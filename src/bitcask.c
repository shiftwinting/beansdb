/*
 *  Beansdb - A high available distributed key-value storage system:
 *
 *      http://beansdb.googlecode.com
 *
 *  Copyright 2010 Douban Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Davies Liu <davies.liu@gmail.com>
 *
 */
 
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#include "bitcask.h"
#include "htree.h"
#include "record.h"
#include "diskmgr.h"

#define MAX_BUCKET_COUNT 256

const uint32_t MAX_RECORD_SIZE = 50 << 20; // 50M
const uint32_t MAX_BUCKET_SIZE = (uint32_t)(4000 << 20); // 4G
const uint32_t WRITE_BUFFER_SIZE = 2 << 20; // 2M

const int SAVE_HTREE_LIMIT = 5;

const char DATA_FILE[] = "%03d.data";
const char HINT_FILE[] = "%03d.hint.qlz";
const char HTREE_FILE[] = "%03d.htree";

struct bitcask_t {
    uint32_t depth, pos;
    time_t before;
    Mgr    *mgr;
    HTree  *tree, *curr_tree;
    int    last_snapshot;
    uint64_t bytes, curr_bytes;
    uint32_t curr;
    char   *write_buffer;
    time_t last_flush_time;
    uint32_t    wbuf_size, wbuf_start_pos, wbuf_curr_pos;
    pthread_mutex_t flush_lock, buffer_lock, write_lock;
};

Bitcask* bc_open(const char* path, int depth, int pos, time_t before)
{
    if (path == NULL || depth > 4) return NULL;
    if (0 != access(path, F_OK) && 0 != mkdir(path, 0750)){
        fprintf(stderr, "mkdir %s failed\n", path);
        return NULL;
    }
    const char* t[] = {path};
    Mgr *mgr = mgr_create(t, 1);
    if (mgr == NULL) return NULL;

    Bitcask* bc = bc_open2(mgr, depth, pos, before);
    if (bc != NULL) bc_scan(bc);
    return bc;
}

Bitcask* bc_open2(Mgr *mgr, int depth, int pos, time_t before)
{
    Bitcask* bc = (Bitcask*)malloc(sizeof(Bitcask));
    if (bc == NULL) return NULL;

    memset(bc, 0, sizeof(Bitcask));    
    bc->mgr = mgr;
    bc->depth = depth;
    bc->pos = pos;
    bc->before = before;
    bc->bytes = 0;
    bc->curr_bytes = 0;
    bc->tree = NULL;
    bc->last_snapshot = -1;
    bc->curr_tree = ht_new(depth, pos);
    bc->wbuf_size = 1024 * 4;
    bc->write_buffer = malloc(bc->wbuf_size);
    bc->last_flush_time = time(NULL);
    pthread_mutex_init(&bc->buffer_lock, NULL);
    pthread_mutex_init(&bc->write_lock, NULL);
    pthread_mutex_init(&bc->flush_lock, NULL);
    return bc;
}

inline char *gen_path(char *dst, const char *base, const char *fmt, int i)
{
    static char path[256];
    char name[16];
    if (dst == NULL) dst = path;
    sprintf(name, fmt, i);
    sprintf(dst, "%s/%s",  base, name);
    return dst;
}

inline bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void skip_empty_file(Bitcask* bc)
{
    int i, last=0;
    char opath[255], npath[255];
    
    const char* base = mgr_base(bc->mgr);
    for (i=0; i<MAX_BUCKET_COUNT; i++) {
        if (file_exists(gen_path(opath, base, DATA_FILE, i))) {
            if (i != last) {
                mgr_rename(opath, gen_path(npath, base, DATA_FILE, last));
                
                if (file_exists(gen_path(opath, base, HINT_FILE, i))) {
                    mgr_rename(opath, gen_path(npath, base, HINT_FILE, last));
                }
                
                if (file_exists(gen_path(opath, base, HTREE_FILE, i))) {
                    mgr_rename(opath, gen_path(npath, base, HTREE_FILE, last));
                }
            }
            last ++;
        }
    }
}

void bc_scan(Bitcask* bc)
{
    char dname[20], hname[20], datapath[255], hintpath[255];
    int i=0;
    struct stat st, hst;
    
    skip_empty_file(bc);

    const char* base = mgr_base(bc->mgr);
    // load snapshot of htree
    for (i=MAX_BUCKET_COUNT-1; i>=0; i--) {
        if (stat(gen_path(datapath, base, HTREE_FILE, i), &st) == 0 
                && stat(gen_path(hintpath, base, HINT_FILE, i), &hst) == 0 
                && st.st_mtime >= hst.st_mtime
                && (bc->before == 0 || st.st_mtime < bc->before)) {
            bc->tree = ht_open(bc->depth, bc->pos, datapath);
            if (bc->tree != NULL) {
                bc->last_snapshot = i;
                break;
            } else {
                fprintf(stderr, "open HTree from %s failed\n", datapath);
                mgr_unlink(datapath);
            }
        }
    }
    if (bc->tree == NULL) {
        bc->tree = ht_new(bc->depth, bc->pos);
    }

    for (i=0; i<MAX_BUCKET_COUNT; i++) {
        if (stat(gen_path(datapath, base, DATA_FILE, i), &st) != 0) {
            break;
        }
        bc->bytes += st.st_size;
        if (i <= bc->last_snapshot) continue;

        gen_path(hintpath, base, HINT_FILE, i);
        if (bc->before == 0){
            if (0 == stat(hintpath, &st)){
                scanHintFile(bc->tree, i, hintpath, NULL);
            }else{
                sprintf(hname, HINT_FILE, i);
                sprintf(hintpath, "%s/%s", mgr_alloc(bc->mgr, hname), hname);
                scanDataFile(bc->tree, i, datapath, hintpath);                
            }
        }else{
            if (0 == stat(hintpath, &st) && 
                (st.st_mtime < bc->before || 0 == stat(datapath, &st) && st.st_mtime < bc->before)){
                scanHintFile(bc->tree, i, hintpath, NULL); 
            }else{
                scanDataFileBefore(bc->tree, i, datapath, bc->before);
            }
        }
    }

    if (i - bc->last_snapshot > SAVE_HTREE_LIMIT) {
        sprintf(dname, HTREE_FILE, i-1);
        sprintf(datapath, "%s/%s", mgr_alloc(bc->mgr, dname), dname);
        if (ht_save(bc->tree, datapath) == 0) {
            sprintf(dname, HTREE_FILE, bc->last_snapshot);
            sprintf(datapath, "%s/%s", base, dname);
            mgr_unlink(datapath);

            bc->last_snapshot = i-1;
        } else {
            fprintf(stderr, "save HTree to %s failed\n", datapath);
        }
    }
    
    bc->curr = i;
}

/*
 * bc_close() is not thread safe, should stop other threads before call it.
 * */
void bc_close(Bitcask *bc)
{
    int i=0;
    char dname[20], hname[20], datapath[255], hintpath[255];
    
    pthread_mutex_lock(&bc->write_lock);
    
    bc_flush(bc, 0, 0);
    
    if (NULL != bc->curr_tree) {
        if (bc->curr_bytes > 0) {
            sprintf(hname, HINT_FILE, bc->curr);
            sprintf(hintpath, "%s/%s", mgr_alloc(bc->mgr, hname), hname);
            build_hint(bc->curr_tree, hintpath);
        }else{
            ht_destroy(bc->curr_tree);
        }
        bc->curr_tree = NULL;
    }

    if (bc->curr_bytes == 0) bc->curr --;
    if (bc->curr - bc->last_snapshot >= SAVE_HTREE_LIMIT) {
        const char* path = mgr_base(bc->mgr);
        sprintf(dname, HTREE_FILE, bc->curr);
        sprintf(datapath, "%s/%s", mgr_alloc(bc->mgr, dname), dname);
        if (ht_save(bc->tree, datapath) == 0) {
            sprintf(dname, HTREE_FILE, bc->last_snapshot);
            sprintf(datapath, "%s/%s", path, dname);
            mgr_unlink(datapath);
        } else {
            fprintf(stderr, "save HTree to %s failed\n", datapath);
        }
    }
    ht_destroy(bc->tree);

    mgr_destroy(bc->mgr);
    free(bc->write_buffer);
    free(bc);
}

uint64_t data_file_size(Bitcask *bc, int bucket) {
    char path[255];
    gen_path(path, mgr_base(bc->mgr), DATA_FILE, bucket);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return 0;
    fseek(fp, 0, 2);
    uint64_t size = ftell(fp);
    fclose(fp);
    return size;
}

void bc_optimize(Bitcask *bc, int limit)
{
    int i, total, last = -1;
    const char *base = mgr_base(bc->mgr);
    // remove htree
    for (i=0; i < bc->curr; i++) {
        mgr_unlink(gen_path(NULL, base, HTREE_FILE, i));
    }
    bc->last_snapshot = -1;

    for (i=0; i < bc->curr; i++) {
        char datapath[255], hintpath[255];
        gen_path(datapath, base, DATA_FILE, i); 
        gen_path(hintpath, base, HINT_FILE, i); 
        int deleted = count_deleted_record(bc->tree, i, hintpath, &total);
        uint64_t dsize = data_file_size(bc, i) * (total - deleted) / total; // guess
        uint64_t last_size = data_file_size(bc, last);
        if (deleted <= total * 0.1 && deleted <= limit 
            && (last == -1 || dsize + last_size > MAX_BUCKET_SIZE)) {
            fprintf(stderr, "only %d records deleted in %d, skip %s\n", deleted, total, datapath);
            last = i;
            continue;
        }

        // last data file size
        uint32_t recoverd = 0;
        if (last >= 0 && last_size + dsize < MAX_BUCKET_SIZE) {
            char ldpath[255], lhpath[255];
            gen_path(ldpath, base, DATA_FILE, last);
            gen_path(lhpath, base, HINT_FILE, last);
            recoverd = optimizeDataFile(bc->tree, i, datapath, hintpath, 
                    limit, MAX_BUCKET_SIZE, last, ldpath, lhpath);
        }
        if (recoverd == 0) {
            recoverd = optimizeDataFile(bc->tree, i, datapath, hintpath, 
                    limit, MAX_BUCKET_SIZE, i, NULL, NULL);
            if (data_file_size(bc, i) == 0) {
                mgr_unlink(datapath);
                mgr_unlink(hintpath);
            } else {
                last = i;
            }
        }
        if (recoverd < 0) {
            break;
        }
        
        pthread_mutex_lock(&bc->buffer_lock);
        bc->bytes -= recoverd;
        pthread_mutex_unlock(&bc->buffer_lock);
    }
}

DataRecord* bc_get(Bitcask *bc, const char* key)
{
    Item *item = ht_get(bc->tree, key);
    if (NULL == item) return NULL;
    if (item->ver < 0){
        free(item);
        return NULL;
    }
    
    uint32_t bucket = item->pos & 0xff;
    uint32_t pos = item->pos & 0xffffff00;
    if (bucket > bc->curr) {
        fprintf(stderr, "BUG: invalid bucket %d > %d\n", bucket, bc->curr);
        ht_remove(bc->tree, key);
        free(item);
        return NULL;
    }

    DataRecord* r = NULL;
    if (bucket == bc->curr) {
        pthread_mutex_lock(&bc->buffer_lock);
        if (bucket == bc->curr && pos >= bc->wbuf_start_pos){
            uint32_t p = pos - bc->wbuf_start_pos;
            r = decode_record(bc->write_buffer + p, bc->wbuf_curr_pos - p, true);
        }
        pthread_mutex_unlock(&bc->buffer_lock);
        
        if (r != NULL){
            free(item);
            return r;
        }
    }
        
    char fname[20], data[255];
    const char * path = mgr_base(bc->mgr);
    sprintf(fname, DATA_FILE, bucket);
    sprintf(data, "%s/%s", path, fname);
    int fd = open(data, O_RDONLY);
    if (-1 == fd){
        goto GET_END;
    }
    
    r = fast_read_record(fd, pos, true);
    if (NULL == r){
        fprintf(stderr, "Bug: get %s failed in %s %u %u\n", key, path, bucket, pos); 
    }else{
         // check key
        if (strcmp(key, r->key) != 0){
            fprintf(stderr, "Bug: record %s is not expected %s in %u @ %u\n", r->key, key, bucket, pos);
            free_record(r);
            r = NULL;
        } 
    }
GET_END:
    if (NULL == r)
        ht_remove(bc->tree, key);
    if (fd != -1) close(fd);
    free(item);
    return r;
}

struct build_thread_args {
    HTree *tree;
    char *path;
};

void* build_thread(void *param)
{
    struct build_thread_args *args = (struct build_thread_args*) param;
    build_hint(args->tree, args->path);
    free(args->path);
    free(param);
    return NULL;
}

void bc_rotate(Bitcask *bc) {
    // build in new thread
    char hname[20], hintpath[255];
    sprintf(hname, HINT_FILE, bc->curr);
    sprintf(hintpath, "%s/%s", mgr_alloc(bc->mgr, hname), hname);
    struct build_thread_args *args = (struct build_thread_args*)malloc(
            sizeof(struct build_thread_args));
    args->tree = bc->curr_tree;
    args->path = strdup(hintpath);
    pthread_t build_ptid;
    pthread_create(&build_ptid, NULL, build_thread, args);
    // next bucket
    bc->curr ++;
    bc->curr_tree = ht_new(bc->depth, bc->pos);
    bc->wbuf_start_pos = 0;
}

void bc_flush(Bitcask *bc, int limit, int flush_period)
{
    if (bc->curr >= MAX_BUCKET_COUNT) {
        fprintf(stderr, "reach max bucket count\n");
        exit(1);
    }
    
    pthread_mutex_lock(&bc->flush_lock);

    time_t now = time(NULL);
    if (bc->wbuf_curr_pos > limit * 1024 || 
        now > bc->last_flush_time + flush_period && bc->wbuf_curr_pos > 0) {
        char name[20], buf[255];
        sprintf(name, DATA_FILE, bc->curr);
        sprintf(buf, "%s/%s", mgr_alloc(bc->mgr, name), name);

        FILE *f = fopen(buf, "ab");
        if (f == NULL) {
            fprintf(stderr, "open file %s for flushing failed.\n", buf);
            exit(1);
        }
        // check file size
        uint64_t last_pos = ftell(f);
        if (last_pos > 0 && last_pos != bc->wbuf_start_pos) {
            fprintf(stderr, "last pos not match: %llu != %d in %s\n", last_pos, bc->wbuf_start_pos, buf);
            exit(1);
        }
      
        pthread_mutex_lock(&bc->buffer_lock);
        int size = bc->wbuf_curr_pos;
        char * tmp = (char*) malloc(size);
        memcpy(tmp, bc->write_buffer, size);
        pthread_mutex_unlock(&bc->buffer_lock);
        
        int n = fwrite(tmp, 1, size, f);
        if (n <= 0) {
            fprintf(stderr, "write failed: return %d\n", n);
            exit(1);
        }
        free(tmp);
        fclose(f);
        bc->last_flush_time = now;

        pthread_mutex_lock(&bc->buffer_lock);
        bc->bytes += n;
        bc->curr_bytes += n;
        if (n < bc->wbuf_curr_pos) {
            memmove(bc->write_buffer, bc->write_buffer + n, bc->wbuf_curr_pos - n);
        }
        bc->wbuf_start_pos += n;
        bc->wbuf_curr_pos -= n;
        if (bc->wbuf_curr_pos == 0) {
            if (bc->wbuf_size < WRITE_BUFFER_SIZE) {
                bc->wbuf_size *= 2;
                free(bc->write_buffer);
                bc->write_buffer = malloc(bc->wbuf_size);
            } else if (bc->wbuf_size > WRITE_BUFFER_SIZE * 2) {
                bc->wbuf_size = WRITE_BUFFER_SIZE;
                free(bc->write_buffer);
                bc->write_buffer = malloc(bc->wbuf_size);
            }
        }
        
        if (bc->wbuf_start_pos + bc->wbuf_size > MAX_BUCKET_SIZE) {
            bc_rotate(bc);
        }
        pthread_mutex_unlock(&bc->buffer_lock);
    }
    pthread_mutex_unlock(&bc->flush_lock);
}

bool bc_set(Bitcask *bc, const char* key, char* value, int vlen, int flag, int version)
{
    if (version < 0 && vlen > 0 || vlen > MAX_RECORD_SIZE){
        fprintf(stderr, "invalid set cmd \n");
        return false;
    }

    bool suc = false;
    pthread_mutex_lock(&bc->write_lock);
    
    int oldv = 0, ver = version;
    Item *it = ht_get(bc->tree, key);
    if (it != NULL) {
        oldv = it->ver;
    }
    
    if (version == 0 && oldv > 0){ // replace
        ver = oldv + 1;
    } else if (version == 0 && oldv <= 0){ // add
        ver = -oldv + 1;
    } else if (version < 0 && oldv <= 0) { // delete, not exist
        goto SET_FAIL;
    } else if (version == -1) { // delete
        ver = - abs(oldv) - 1;
    } else if (abs(version) <= abs(oldv)) { // sync
        goto SET_FAIL;
    } else { // sync
        ver = version;
    }
    
    uint16_t hash = gen_hash(value, vlen);
    if (ver < 0) hash = 0;

    if (NULL != it && hash == it->hash) {
        DataRecord *r = bc_get(bc, key);
        if (r != NULL && r->flag == flag && vlen  == r->vsz
             && memcmp(value, r->value, vlen) == 0) {
            if (version != 0){
                // update version
                ht_add(bc->tree, key, it->pos, it->hash, ver);
                if (it->pos & 0xff == bc->curr){
                    if (bc->curr_tree == NULL) {
                        fprintf(stderr, "BUG: curr_tree should not be NULL\n");
                    }else{
                        ht_add(bc->curr_tree, key, it->pos, it->hash, ver);
                    }
                }
            }
            suc = true;
            free_record(r);
            goto SET_FAIL;
        }
        if (r != NULL) free_record(r);
    }
    
    int klen = strlen(key);
    DataRecord *r = malloc(sizeof(DataRecord) + klen);
    r->ksz = klen;
    memcpy(r->key, key, klen);
    r->vsz = vlen;
    r->value = value;
    r->free_value = false;
    r->flag = flag;
    r->version = ver;
    r->tstamp = time(NULL);

    int rlen;
    char *rbuf = encode_record(r, &rlen);
    if (rbuf == NULL || (rlen & 0xff) != 0){
        fprintf(stderr, "encode_record() failed with %d\n", rlen);
        if (rbuf != NULL) free(rbuf);
        goto SET_FAIL; 
    }

    pthread_mutex_lock(&bc->buffer_lock);
    // record maybe larger than buffer
    if (bc->wbuf_curr_pos + rlen > bc->wbuf_size) {
        pthread_mutex_unlock(&bc->buffer_lock);
        bc_flush(bc, 0, 0);
        pthread_mutex_lock(&bc->buffer_lock);
        
        while (rlen > bc->wbuf_size) {
            bc->wbuf_size *= 2;
            free(bc->write_buffer);
            bc->write_buffer = malloc(bc->wbuf_size);
        }
        if (bc->wbuf_start_pos + bc->wbuf_size > MAX_BUCKET_SIZE) {
            bc_rotate(bc);
        }
    }
    memcpy(bc->write_buffer + bc->wbuf_curr_pos, rbuf, rlen);
    int pos = (bc->wbuf_start_pos + bc->wbuf_curr_pos) | bc->curr;
    bc->wbuf_curr_pos += rlen;
    pthread_mutex_unlock(&bc->buffer_lock);
   
    ht_add(bc->tree, key, pos, hash, ver);
    ht_add(bc->curr_tree, key, pos, hash, ver);
    suc = true;
    free(rbuf);
    free_record(r);

SET_FAIL:
    pthread_mutex_unlock(&bc->write_lock);
    if (it != NULL) free(it);
    return suc;
}

bool bc_delete(Bitcask *bc, const char* key)
{
    return bc_set(bc, key, "", 0, 0, -1);
}

uint16_t bc_get_hash(Bitcask *bc, const char * pos, int *count)
{
    return ht_get_hash(bc->tree, pos, count);
}

char* bc_list(Bitcask *bc, const char* pos, const char* prefix)
{
    return ht_list(bc->tree, pos, prefix);
}

uint32_t   bc_count(Bitcask *bc, uint32_t* curr)
{
    uint32_t total = 0;
    ht_get_hash(bc->tree, "@", &total);
    if (NULL != curr && NULL != bc->curr_tree) {
        ht_get_hash(bc->curr_tree, "@", curr);
    }
    return total;
}

void bc_stat(Bitcask *bc, uint64_t *bytes)
{
    if (bytes != NULL) {
        *bytes = bc->bytes;
    }
}