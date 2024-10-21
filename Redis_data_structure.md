# Redis basic data structure

## SDS

- SDS 是Redis用于管理字符串的最底层数据结构
- SDS 理解为一片连续内存  header + data的柔性数组
- 以’\0’结尾，使其兼容部分C字符串函数

```C
# in sds.h, sds 就是一个char* 的指针， sds指针直接指向数据
typedef char *sds;

#define SDS_TYPE_5  0
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4

struct __attribute__ ((__packed__)) sdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];     // here is where sds is point to， 柔性数组，为空时不占内存
};

// flags 用于表示 SDS_TYPE, 因为有5个选项，所以至少需要3bit
// redis 获取 SDS的type的方式
unsigned char flags = s[-1];


// 惰性删除 减少了free()的调用
sds sdstrim(sds s, const char *cset) {
    char *start, *end, *sp, *ep;
    size_t len;

    sp = start = s;
    ep = end = s+sdslen(s)-1;
    while(sp <= end && strchr(cset, *sp)) sp++;
    while(ep > sp && strchr(cset, *ep)) ep--;
    len = (sp > ep) ? 0 : ((ep-sp)+1);
    if (s != sp) memmove(s, sp, len);
    s[len] = '\0';
    sdssetlen(s,len);
    return s;
}
```

## List
双向链表

adlist.c

```C
typedef struct listNode {
    struct listNode *prev;
    struct listNode *next;
    void *value;
} listNode;

typedef struct listIter {
    listNode *next;
    int direction;
} listIter;

typedef struct list {
    listNode *head;
    listNode *tail;
    void *(*dup)(void *ptr);   // void * means return type; (*dup) means a function pointer named dup; (void *ptr) shows the parameter
    void (*free)(void *ptr);
    int (*match)(void *ptr, void *key);
    unsigned long len;
} list;

```

## dict

dict.c dict.h
Redis的 哈希表实现

### Data-structure
```C
typedef struct dictEntry {
    void *key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next;
} dictEntry;  /*单个hash表节点， 用链地址处理hash冲突， 头插*/

typedef struct dictht {
    dictEntry **table;    
    unsigned long size;  // 表大小
    unsigned long sizemask;  // hash表大小为2的幂次，求hash值的时候需要对 idx = idx & sizemask; 来确定key在表中的位置，用 &加速，而不用取余运算
    unsigned long used;  // 已经使用个数
} dictht;   // actual dict implement

typedef struct dictType {
    uint64_t (*hashFunction)(const void *key);   // 对key的 hash function
    void *(*keyDup)(void *privdata, const void *key);
    void *(*valDup)(void *privdata, const void *obj);
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);
    void (*keyDestructor)(void *privdata, void *key);
    void (*valDestructor)(void *privdata, void *obj);
} dictType;

typedef struct dict {
    dictType *type;  /*相关的函数操作*/
    void *privdata;
    dictht ht[2];   /* 两个hash-table, 用处：implement incremental rehashing, for the old to the new table*/
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */
    unsigned long iterators; /* number of iterators currently running */
} dict;

```

### Operation
```C
// 外部接口函数


// 初始化
dict *dictCreate(dictType *type, void *privDataPtr);
//ht在第一次初始化时只会启用ht[0],ht[1]在整个dict扮演着临时存储的作用

/* Add */ 
/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as he wishes.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    dictht *ht;

    if (dictIsRehashing(d)) _dictRehashStep(d);

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];   // During rehash, add to ht[1]
    entry = zmalloc(sizeof(*entry));
    entry->next = ht->table[index];
    ht->table[index] = entry;
    ht->used++;

    /* Set the hash entry fields. */
    dictSetKey(d, entry, key);
    return entry;
}

/* expand 
只在add value的时候确定是否需要扩容并rehash

dictAddRaw -> _dictKeyIndex -> _dictExpandIfNeeded
 */ 
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio))
    {
        return dictExpand(d, d->ht[0].used*2);
    }
    return DICT_OK;
}

/* Resize 
缩容
ServerCron -> databasesCron -> tryResizeHashTables -> dictResize
*/
int dictResize(dict *d)
{
    int minimal;

    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
    minimal = d->ht[0].used;
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    return dictExpand(d, minimal);
}

/* Remove */ 
/*

int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

dictEntry *dictUnlink(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,1);
}
Remove an element from the table, but without actually releasing
the key, value and dictionary entry. The dictionary entry is returned
if the element was found (and unlinked from the table), and the user
should later call `dictFreeUnlinkedEntry()` with it in order to release it.


*/
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    if (d->ht[0].used == 0 && d->ht[1].used == 0) return NULL;

    if (dictIsRehashing(d)) _dictRehashStep(d);
    h = dictHashKey(d, key);

    for (table = 0; table <= 1; table++) {  // 在rehash的时候 两个table的都要删除，无法确定table[0]的是否放入 table[1]
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        prevHe = NULL;
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list */
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;
                if (!nofree) {
                    dictFreeKey(d, he);  // 调用 d-> type-> keyDestructor
                    dictFreeVal(d, he);  // 调用 d-> type-> valDestructor
                    zfree(he);
                }
                d->ht[table].used--;
                return he;
            }
            prevHe = he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
    return NULL; /* not found */
}

/* Rehash */
/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
    if (!dictIsRehashing(d)) return 0;

    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        de = d->ht[0].table[d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
        while(de) {
            uint64_t h;

            nextde = de->next;
            /* Get the index in the new hash table */
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;
            d->ht[0].used--;
            d->ht[1].used++;
            de = nextde;
        }
        d->ht[0].table[d->rehashidx] = NULL;
        d->rehashidx++;
    }

    /* Check if we already rehashed the whole table... */
    if (d->ht[0].used == 0) {
        zfree(d->ht[0].table);
        d->ht[0] = d->ht[1];
        _dictReset(&d->ht[1]);
        d->rehashidx = -1;
        return 0;
    }

    /* More to rehash... */
    return 1;
}

/*
    没有 rehash的时候，数据均放入 ht[0]
    rehash的过程中就是不断把ht[0]的东西放入 ht[1],
    结合 dictAddRaw 函数可以看到
    在 rehash 中，新增加的k-v会放入 ht[1]， 所以在get_element的时候， 两个表都要查看
    完成 rehash 后， ht[1]会move给ht[0], ht[1]会置空

    执行rehash的事件：
    1. dict的相关操作
    2. ServerCron -> databasesCron -> incrementallyRehash -> dictRehash
*/
```

### iterator
```C

typedef struct dictIterator {
    dict *d;
    long index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint;
} dictIterator;

dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        if (iter->entry == NULL) {
            dictht *ht = &iter->d->ht[iter->table];
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe)
                    iter->d->iterators++;   // 非安全模式下，只支持只读操作，使用字典的删除、添加、查找等方法会造成不可预期的问题，如重复遍历元素或者漏掉元素，但支持rehash操作
                else
                    iter->fingerprint = dictFingerprint(iter->d);
            }
            iter->index++;
            if (iter->index >= (long) ht->size) {
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                } else {
                    break;
                }
            }
            iter->entry = ht->table[iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}

/* This function performs just a step of rehashing, and only if there are
 * no safe iterators bound to our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
static void _dictRehashStep(dict *d) {
    if (d->iterators == 0) dictRehash(d,1);
}

// 因此 safe iterator不会看到重复元素， 而unsafe的iterator在执行中任然可以执行rehash
// 可能存在在table[0]中读取元素A后， A被rehash到table[1]中 又被继续读取的情况
```
## intset
intset.c
intset.h

用于保存整数值的集合抽象数据结构，他可以保存类型为16、32或者64位的整数值，且保证集合中不会出现重复元素,数据也是从小到大存储
```c

// encoding 类型
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

typedef struct intset {
    uint32_t encoding;
    uint32_t length;
    int8_t contents[];
} intset;
```
1. 提升灵活性
可以通过自动升级底层数组来适应新元素，所以可以将任意类型的整数添加至集合，而不必担心类型错误
2. 节约内存
不同类型采用不同的类型的空间对其存储，从而避免空间浪费
- 降级：不支持降级
- 添加和删除
均需要进行remalloc操作，因此慎用。

## ziplist
ziplist.c
ziplist.h

由一系列特殊编码的连续内存块组成的顺序型数据结构，一个压缩列表可以包含任意多个节点，每个节点可以保存一个字节数组或者一个整数值。适合存储小对象和长度有限的数据。
具体编码可以参考注释
1. 查找的时间复杂度为O(N)


## redisObject
Redis的抽象存储对象， key-value pair中， 一个key就是一个 redisObject, 忽略其底层数据结构，用同一个类型进行抽象

```C
/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define OBJ_ENCODING_RAW 0     /* Raw representation */
#define OBJ_ENCODING_INT 1     /* Encoded as integer */
#define OBJ_ENCODING_HT 2      /* Encoded as hash table */
#define OBJ_ENCODING_ZIPMAP 3  /* Encoded as zipmap */
#define OBJ_ENCODING_LINKEDLIST 4 /* No longer used: old list encoding. */
#define OBJ_ENCODING_ZIPLIST 5 /* Encoded as ziplist */
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset */
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist */
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding */
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of ziplists */
#define OBJ_ENCODING_STREAM 10 /* Encoded as a radix tree of listpacks */

typedef struct redisObject {
    unsigned type:4;
    unsigned encoding:4;
    unsigned lru:LRU_BITS; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time). */
    int refcount;
    void *ptr;
} robj;

```

## Str 字符串

Redis 命令的parse:t_string.c   

#define OBJ_ENCODING_RAW 0     /* Raw representation */
#define OBJ_ENCODING_INT 1     /* Encoded as integer */
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding */

see object.c:tryObjectEncoding

- OBJ_ENCODING_INT：sdslen(s) <= 20 时， 考虑将字符转为int(因为2的64次方在10进制下至少有21位)
- OBJ_ENCODING_EMBSTR： embeded string, 
    ```C
        robj *createEmbeddedStringObject(const char *ptr, size_t len) {
            robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr8)+len+1);
            struct sdshdr8 *sh = (void*)(o+1);
            //...
            o->type = OBJ_STRING;
            o->encoding = OBJ_ENCODING_EMBSTR;
            o->ptr = sh+1;
            o->refcount = 1;
        }
    ```
    sds与 robj放在连续内存里面， 原因： * The current limit of 44 is chosen so that the biggest string object
    we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc.

- OBJ_ENCODING_RAW: 将sds字符串放到外部
    ```C
        /* Optimize the SDS string inside the string object to require little space,
        * in case there is more than 10% of free space at the end of the SDS
        * string. This happens because SDS strings tend to overallocate to avoid
        * wasting too much time in allocations when appending to the string. */
        void trimStringObjectIfNeeded(robj *o) {
            if (o->encoding == OBJ_ENCODING_RAW &&
                sdsavail(o->ptr) > sdslen(o->ptr)/10)
            {
                o->ptr = sdsRemoveFreeSpace(o->ptr);
            }
        }
    ```

## QUICKLIST
quicklist.c  quicklist.h
```C
/* quicklistNode is a 32 byte struct describing a ziplist for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * count: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, NONE=1, ZIPLIST=2.
 * recompress: 1 bit, bool, true if node is temporarry decompressed for usage.
 * attempted_compress: 1 bit, boolean, used for verifying during testing.
 * extra: 10 bits, free for future use; pads out the remainder of 32 bits */
typedef struct quicklistNode {
    struct quicklistNode *prev;
    struct quicklistNode *next;
    unsigned char *zl;
    unsigned int sz;             /* ziplist size in bytes */
    unsigned int count : 16;     /* count of items in ziplist */
    unsigned int encoding : 2;   /* RAW==1 or LZF==2 */
    unsigned int container : 2;  /* NONE==1 or ZIPLIST==2 */
    unsigned int recompress : 1; /* was this node previous compressed? */
    unsigned int attempted_compress : 1; /* node can't compress; too small */
    unsigned int extra : 10; /* more bits to steal for future usage */
} quicklistNode;

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->sz.
 * When quicklistNode->zl is compressed, node->zl points to a quicklistLZF */
typedef struct quicklistLZF {
    unsigned int sz; /* LZF size in bytes*/
    char compressed[];
} quicklistLZF;

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: -1 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor. */
typedef struct quicklist {
    quicklistNode *head;
    quicklistNode *tail;
    unsigned long count;        /* total count of all entries in all ziplists */
    unsigned long len;          /* number of quicklistNodes */
    int fill : 16;              /* fill factor for individual nodes */
    unsigned int compress : 16; /* depth of end nodes not to compress;0=off */
} quicklist;

typedef struct quicklistEntry {
    //指向所属的quicklist指针
    const quicklist *quicklist;
    //指向所属的quicklistNode节点的指针
    quicklistNode *node;
    //指向当前ziplist结构的指针
    unsigned char *zi;
    //指向当前ziplist结构的字符串value成员
    unsigned char *value;
    //指向当前ziplist结构的整型value成员
    long long longval;
    //当前ziplist结构的字节数
    unsigned int sz;
    //保存相对ziplist的偏移量
    int offset;
} quicklistEntry;
```
- 双向链表，插入和删除效率高，但是对内存不友好，而且需要存取额外的前后两个指针， 数组为代表的连续内存，插入和删除时间复杂度高，但是对内存友好(局部性原理)，因此综合了两个，催生了quicklist数据结构，其实在C++的stl中deque也是这种设计模式
- fill


## hash
Redis的hash对象，采用了dict 和 ziplist的方式来实现
  - 由ziplist转dict的操作是不可逆的。
  - 尽可能的使用ziplist来作为hash底层实现。长度尽量控制在1000以内，否则由于存取操作时间复杂度O(n)，长列表会导致CPU消耗严重。对象也不要太大。
  - 两个参数【hash-max-ziplist-entries和hash-max-ziplist-value】可在配置文件中进行修改

## set
t_set.c
采用了dict 和 intset 的方式来实现
  - 由intset转dict的操作是不可逆的。
  - set是不允许重复的。
  - 支持交并补
  - 参数【set-max-intset-entries】可在配置文件中进行修改
  - dict作为底层对象时，value值为null
  - intset作为底层对象时，其查找的时间复杂度为O(logN)

## zset 有序集合  zskiplist
t_zset.c
```C
typedef struct zset {
    dict *dict;
    zskiplist *zsl;
} zset;
/* 在skiplist的基础上，还需要创建dict的原因是当需要获取某个元素的score时，skplist的时间复杂度为O(lgn)，而dict时间复杂度为O(1).（见zsetAdd）。需要特别注意的是当底层为ziplist时，该操作依旧为O(n) */

```
- skiplist和dict共享元素和分值（指针复制）。
- 由ziplist转skiplist的操作是不可逆的。
- 两个参数【zset-max-ziplist-entries和zset-max-ziplist-value 】可在配置文件中进行修改。
- zset也不允许重复。

zadd user:ranking:2016_03_15     3         mike
     |--------set name-----| |-score-|  |--key--| 


## Client
每一个链接过来，就会创造一个client对象

```C
typedef struct client {
    uint64_t id;            /* Client incremental unique ID. */
    int fd;                 /* Client socket. */
    redisDb *db;            /* Pointer to currently SELECTed DB. */
    robj *name;             /* As set by CLIENT SETNAME. */
    sds querybuf;           /* Buffer we use to accumulate client queries. */
    size_t qb_pos;          /* The position we have read in querybuf. */
    sds pending_querybuf;   /* If this client is flagged as master, this buffer
                               represents the yet not applied portion of the
                               replication stream that we are receiving from
                               the master. */
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size. */
    int argc;               /* Num of arguments of current command. */
    robj **argv;            /* Arguments of current command. */
    struct redisCommand *cmd, *lastcmd;  /* Last command executed. */
    int reqtype;            /* Request protocol type: PROTO_REQ_* */
    int multibulklen;       /* Number of multi bulk arguments left to read. */
    long bulklen;           /* Length of bulk argument in multi bulk request. */
    list *reply;            /* List of reply objects to send to the client. */
    unsigned long long reply_bytes; /* Tot bytes of objects in reply list. */
    size_t sentlen;         /* Amount of bytes already sent in the current
                               buffer or object being sent. */
    time_t ctime;           /* Client creation time. */
    time_t lastinteraction; /* Time of the last interaction, used for timeout */
    time_t obuf_soft_limit_reached_time;
    int flags;              /* Client flags: CLIENT_* macros. */
    int authenticated;      /* When requirepass is non-NULL. */
    int replstate;          /* Replication state if this is a slave. */
    int repl_put_online_on_ack; /* Install slave write handler on first ACK. */
    int repldbfd;           /* Replication DB file descriptor. */
    off_t repldboff;        /* Replication DB file offset. */
    off_t repldbsize;       /* Replication DB file size. */
    sds replpreamble;       /* Replication DB preamble. */
    long long read_reploff; /* Read replication offset if this is a master. */
    long long reploff;      /* Applied replication offset if this is a master. */
    long long repl_ack_off; /* Replication ack offset, if this is a slave. */
    long long repl_ack_time;/* Replication ack time, if this is a slave. */
    long long psync_initial_offset; /* FULLRESYNC reply offset other slaves
                                       copying this slave output buffer
                                       should use. */
    char replid[CONFIG_RUN_ID_SIZE+1]; /* Master replication ID (if master). */
    int slave_listening_port; /* As configured with: SLAVECONF listening-port */
    char slave_ip[NET_IP_STR_LEN]; /* Optionally given by REPLCONF ip-address */
    int slave_capa;         /* Slave capabilities: SLAVE_CAPA_* bitwise OR. */
    multiState mstate;      /* MULTI/EXEC state */
    int btype;              /* Type of blocking op if CLIENT_BLOCKED. */
    blockingState bpop;     /* blocking state */
    long long woff;         /* Last write global replication offset. */
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS */
    dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) */
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) */
    sds peerid;             /* Cached peer ID. */
    listNode *client_list_node; /* list node in client list */

    /* Response buffer */
    int bufpos;
    char buf[PROTO_REPLY_CHUNK_BYTES];
} client;

```
客户端关闭
1. 调用client kill命令
2. 不符合协议格式的命令
3. 客户端超时 clientsCron中检测
```C
int clientsCronHandleTimeout(client *c, mstime_t now_ms) {
    time_t now = now_ms/1000;

    if (server.maxidletime &&
        !(c->flags & CLIENT_SLAVE) &&    /* no timeout for slaves and monitors */
        !(c->flags & CLIENT_MASTER) &&   /* no timeout for masters */
        !(c->flags & CLIENT_BLOCKED) &&  /* no timeout for BLPOP */
        !(c->flags & CLIENT_PUBSUB) &&   /* no timeout for Pub/Sub clients */
        (now - c->lastinteraction > server.maxidletime))
    {
        serverLog(LL_VERBOSE,"Closing idle client");
        freeClient(c);
        return 1;
    }
```
1. 输入缓冲区超过阈值1G,受参数`RedisServer的client_max_querybuf_len`控制

```C
// client command
"id                     -- Return the ID of the current connection.",
"getname                -- Return the name of the current connection.",
"kill <ip:port>         -- Kill connection made from <ip:port>.",
"kill <option> <value> [option value ...] -- Kill connections. Options are:",
"     addr <ip:port>                      -- Kill connection made from <ip:port>",
"     type (normal|master|replica|pubsub) -- Kill connections by type.",
"     skipme (yes|no)   -- Skip killing current connection (default: yes).",
"list [options ...]     -- Return information about client connections. Options:",
"     type (normal|master|replica|pubsub) -- Return clients of specified type.",
"pause <timeout>        -- Suspend all Redis clients for <timout> milliseconds.",
"reply (on|off|skip)    -- Control the replies sent to the current connection.",
"setname <name>         -- Assign the name <name> to the current connection.",
"unblock <clientid> [TIMEOUT|ERROR] -- Unblock the specified blocked client."
```

## redis db

```C
typedef struct redisDb {
    dict *dict;                 /* The keyspace for this DB , <robj key, robj val>*/  
    dict *expires;              /* Timeout of keys with a timeout set, <robj key, s64(time to expire)> */
    dict *blocking_keys;        /* Keys with clients waiting for data (BLPOP)*/
    dict *ready_keys;           /* Blocked keys that received a PUSH */
    dict *watched_keys;         /* WATCHED keys for MULTI/EXEC CAS */
    int id;                     /* Database ID */
    long long avg_ttl;          /* Average TTL, just for stats */
    list *defrag_later;         /* List of key names to attempt to defrag one by one, gradually. */
} redisDb;

// keyspace就是各个不同的redisDb, 默认所有的建都放在redisDb->dict中，默认 redis 会有16个redisDb.

robj *lookupKey(redisDb *db, robj *key, int flags) {
    dictEntry *de = dictFind(db->dict,key->ptr);
    if (de) {
        robj *val = dictGetVal(de);

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        if (server.rdb_child_pid == -1 &&
            server.aof_child_pid == -1 &&
            !(flags & LOOKUP_NOTOUCH))
        {
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
                val->lru = LRU_CLOCK();
            }
        }
        return val;
    } else {
        return NULL;
    }
}

// 所有数据都以 robj的形式 放在 dict中，每个robj又由其底层数据结构决定
// skiplist只用于zset !!!!

struct redisServer {
    /* General */
    pid_t pid;                  /* Main process pid. */
    char *configfile;           /* Absolute config file path, or NULL */
    char *executable;           /* Absolute executable file path. */
    char **exec_argv;           /* Executable argv vector (copy). */
    int dynamic_hz;             /* Change hz value depending on # of clients. */
    int config_hz;              /* Configured HZ value. May be different than
                                   the actual 'hz' field value if dynamic-hz
                                   is enabled. */
    int hz;                     /* serverCron() calls frequency in hertz */
    //看这里
    redisDb *db;    // redis server 只有一个instance的 global variable, 这是一个Db的数组
    ...
}
```
#### 数据库切换

在redis-cli 中 输入 SELECT <Db number> 切换数据库

```C
int selectDb(client *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
    c->db = &server.db[id];
    return C_OK;
}
```

# process flow
Redis 命令的parse:t_string.c



network.c : readQueryFromClient   processInputBufferAndReplicate  processInputBuffer(Resp)  processCommand 

-> addReply -> clientInstallWriteHandler "server.clients_pending_write"


添加写事件： before-sleep -> handleClientsWithPendingWrites -> server.clients_pending_write
                                                           -> writeToClient


```C
/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth. */
int handleClientsWithPendingWrites(void) {
    listIter li;
    listNode *ln;
    int processed = listLength(server.clients_pending_write);

    listRewind(server.clients_pending_write,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_WRITE;
        listDelNode(server.clients_pending_write,ln);

        /* If a client is protected, don't do anything,
         * that may trigger write error or recreate handler. */
        if (c->flags & CLIENT_PROTECTED) continue;

        /* Try to write buffers to the client socket. */
        if (writeToClient(c->fd,c,0) == C_ERR) continue;

        /* If after the synchronous writes above we still have data to
         * output to the client, we need to install the writable handler. */
        if (clientHasPendingReplies(c)) {
            int ae_flags = AE_WRITABLE;
            /* For the fsync=always policy, we want that a given FD is never
             * served for reading and writing in the same event loop iteration,
             * so that in the middle of receiving the query, and serving it
             * to the client, we'll call beforeSleep() that will do the
             * actual fsync of AOF to disk. AE_BARRIER ensures that. */
            if (server.aof_state == AOF_ON &&
                server.aof_fsync == AOF_FSYNC_ALWAYS)
            {
                ae_flags |= AE_BARRIER;
            }
            if (aeCreateFileEvent(server.el, c->fd, ae_flags,
                sendReplyToClient, c) == AE_ERR)
            {
                    freeClientAsync(c);
            }
        }
    }
    return processed;
}


/// write_to_client 为什么不把所有的reply 写完呢？
/* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver.
         *
         * Moreover, we also send as much as possible if the client is
         * a slave or a monitor (otherwise, on high-speed traffic, the
         * replication/output buffer will grow indefinitely) */
        if (totwritten > NET_MAX_WRITES_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory) &&
            !(c->flags & CLIENT_SLAVE)) break;
```
流程 ： 

beforesleep

epollwait

aftersleep

handle event


## expire <key> <seconds> 

见函数
expireGenericCommand

- db 有一个存储 expired key的 dict
- 删除策略 定期删除（databaseCron 及 beforeSleep的时候进行定期删除）和惰性删除（在 set, get等命令查到key时将其置为expired）
如expire命令： setExpire(c,c->db,key,when)—->de = dictFind(db->dict,key->ptr)—->dictAddOrFind(db->expires,dictGetKey(de))

- 删除过期时间
persist 

- 查看键剩余时间
查看过期时间：ttl 
lookupKeyReadWithFlags—->getExpire—->ttl = expire-mstime();

### 懒惰删除

set, add 等命令 -> (t_string.c, t_set.c ....) getGenericCommand -> lookupKeyReadOrReply -> lookupKeyReadWithFlags -> expireIfNeeded

```C
/* The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
int expireIfNeeded(redisDb *db, robj *key) {
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a slave, instead of
     * evicting the expired key from the database, we return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    if (server.masterhost != NULL) return 1;

    /* Delete the key */
    server.stat_expiredkeys++;
    propagateExpire(db,key,server.lazyfree_lazy_expire);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,
        "expired",key,db->id);
    return server.lazyfree_lazy_expire ? dbAsyncDelete(db,key) :
                                         dbSyncDelete(db,key);
}
```

### 定期删除

databasesCron -> activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW)

beforeSleep -> activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST)

## BIO 后台线程
close, fsync, free
这个三个线程 与 io-threads 的 io线程不是一回事

### 初始化 与 相关参数
```C
// flow
// sever.c:main() -> InitServerLast() -> bioInit()

/* Exported API and Variable define*/
void bioInit(void);
void bioCreateBackgroundJob(int type, void *arg1, void *arg2, void *arg3);
unsigned long long bioPendingJobsOfType(int type);
unsigned long long bioWaitStepOfType(int type);
time_t bioOlderJobOfType(int type);
void bioKillThreads(void);

/* Background job opcodes */
#define BIO_CLOSE_FILE    0 /* Deferred close(2) syscall. */
#define BIO_AOF_FSYNC     1 /* Deferred AOF fsync. */
#define BIO_LAZY_FREE     2 /* Deferred objects freeing. */
#define BIO_NUM_OPS       3
// 默认会起三个子线程
```

### bio backgroud
```C
void bioInit(void) {

    for (j = 0; j < BIO_NUM_OPS; j++) {
        pthread_mutex_init(&bio_mutex[j],NULL);
        pthread_cond_init(&bio_newjob_cond[j],NULL);
        pthread_cond_init(&bio_step_cond[j],NULL);
        bio_jobs[j] = listCreate(); 
        bio_pending[j] = 0;
        ....
        void *arg = (void*)(unsigned long) j;
        pthread_create(&thread,&attr,bioProcessBackgroundJobs,arg);
        bio_threads[j] = thread;
    }

}

void *bioProcessBackgroundJobs(void *arg) {
    unsigned long type = (unsigned long) arg;
    ...
    pthread_mutex_lock(&bio_mutex[type]);

    while (1) {
        if (listLength(bio_jobs[type]) == 0) { // 这是一个生产者消费者模型
            pthread_cond_wait(&bio_newjob_cond[j]， &bio_mutex[type]);
            continue;
        }
        // get job from the bio_jobs[type]
        pthread_mutex_unlock(&bio_mutex[type]);

        ...

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
        pthread_mutex_lock(&bio_mutex[type]);
        listDelNode(bio_jobs[type],ln);
        bio_pending[type]--;

        /* Unblock threads blocked on bioWaitStepOfType() if any. */
        pthread_cond_broadcast(&bio_step_cond[type]);
    }
}



``` 


# 持久化   

暂时不考虑 MOdule

## RDB
Rdb思想是把当前进程数据生成快照保存到硬盘的过程，保存数据库的键值对

- pros:
1. 文件紧凑，一个二进制文件，适用于备份，全量复制等场景。
2. Redis启动时恢复速度快。
3. 适合做冷备，比如定期将rdb文件复制到远程文件系统中。

- cons:
1. 无法做到实时持久化/秒级持久化。
2. 可能会丢失数据。
3. 兼容性的问题。

### 相关配置、命令

- config：
```
# Save the DB to disk.
#
# save <seconds> <changes> [<seconds> <changes> ...]
#
# Redis will save the DB if the given number of seconds elapsed and it
# surpassed the given number of write operations against the DB.
#
# Snapshotting can be completely disabled with a single empty string argument
# as in following example:
#
save ""

# By default Redis will stop accepting writes if RDB snapshots are enabled
# (at least one save point) and the latest background save failed.
# This will make the user aware (in a hard way) that data is not persisting
# on disk properly, otherwise chances are that no one will notice and some
# disaster will happen.
#
# If the background saving process will start working again Redis will
# automatically allow writes again.
#
# However if you have setup your proper monitoring of the Redis server
# and persistence, you may want to disable this feature so that Redis will
# continue to work as usual even if there are problems with disk,
# permissions, and so forth.

stop-writes-on-bgsave-error yes

# Compress string objects using LZF when dump .rdb databases?
# By default compression is enabled as it's almost always a win.
# If you want to save some CPU in the saving child set it to 'no' but
# the dataset will likely be bigger if you have compressible values or keys.
rdbcompression yes

# Since version 5 of RDB a CRC64 checksum is placed at the end of the file.
# This makes the format more resistant to corruption but there is a performance
# hit to pay (around 10%) when saving and loading RDB files, so you can disable it
# for maximum performances.
#
# RDB files created with checksum disabled have a checksum of zero that will
# tell the loading code to skip the check.
rdbchecksum yes
```

command:

save

bgsave 

### 基本数据结构 与 存储流程

```C
struct saveparam {
    time_t seconds;
    int changes;
};

struct redisServer {
    ...
    /* RDB persistence */
    long long dirty;                /* Changes to DB from the last save */
    long long dirty_before_bgsave;  /* Used to restore dirty on failed BGSAVE */
    pid_t rdb_child_pid;            /* PID of RDB saving child */
    struct saveparam *saveparams;   /* Save points array for RDB */
    int saveparamslen;              /* Number of saving points */
    char *rdb_filename;             /* Name of RDB file */
    int rdb_compression;            /* Use compression in RDB? */
    int rdb_checksum;               /* Use RDB checksum? */
    time_t lastsave;                /* Unix time of last successful save */
    time_t lastbgsave_try;          /* Unix time of last attempted bgsave */
    time_t rdb_save_time_last;      /* Time used by last RDB save run. */
    time_t rdb_save_time_start;     /* Current RDB save start time. */
    int rdb_bgsave_scheduled;       /* BGSAVE when possible if true. */
    int rdb_child_type;             /* Type of save by active child. */
    int lastbgsave_status;          /* C_OK or C_ERR */
    int stop_writes_on_bgsave_err;  /* Don't allow writes if can't BGSAVE */
    int rdb_pipe_write_result_to_parent; /* RDB pipes used to return the state */
    int rdb_pipe_read_result_from_child; /* of each slave in diskless SYNC. */
    /* Pipe and data structures for child -> parent info sharing. */
    int child_info_pipe[2];         /* Pipe used to write the child_info_data. */
    struct {
        int process_type;           /* AOF or RDB child? */
        size_t cow_size;            /* Copy on write size. */
        unsigned long long magic;   /* Magic value to make sure data is valid. */
    } child_info_data;
    ...
}

rdbSaveInfo rsi, *rsiptr;
rsiptr = rdbPopulateSaveInfo(&rsi);

rdbSaveBackground(server.rdb_filename,rsiptr) {

    start = ustime();
    if ((childpid = fork()) == 0) {
        ...
        retval = rdbSave(filename,rsi);  // fork后在子进程调用rdbSave
    }
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
int rdbSave(char *filename, rdbSaveInfo *rsi) {
    ...
    // 创建临时文件
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    
    ...

    if (server.rdb_save_incremental_fsync)
        rioSetAutoSync(&rdb,REDIS_AUTOSYNC_BYTES); 
    /* auto sync, Set the file-based rio object to auto-fsync every 'bytes' file written, This feature is useful in a few contexts since when we rely on OS write
    buffers sometimes the OS buffers way too much, resulting in too many
    disk I/O concentrated in very little time*/

    if (rdbSaveRio(&rdb, &error, RDB_SAVE_NONE, rsi) == C_ERR) {  // rdb save

    }

    /* Make sure data will not remain on the OS's output buffers */
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;
    if (fclose(fp) == EOF) goto werr;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        ...
    }
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns C_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
int rdbSaveRio(rio *rdb, int *error, int flags, rdbSaveInfo *rsi) {

    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
    if (rdbSaveInfoAuxFields(rdb,flags,rsi) == -1) goto werr; /* rdb结构*/
    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_BEFORE_RDB) == -1) goto werr;
    
    // 开始存储数据库信息
    for (j = 0; j < server.dbnum; j++) {
        ...
        if (rdbSaveType(rdb,RDB_OPCODE_SELECTDB) == -1) goto werr;
        if (rdbSaveLen(rdb,j) == -1) goto werr;
        ...
        if (rdbSaveType(rdb,RDB_OPCODE_RESIZEDB) == -1) goto werr;
        if (rdbSaveLen(rdb,db_size) == -1) goto werr;
        if (rdbSaveLen(rdb,expires_size) == -1) goto werr;

        while((de = dictNext(di)) != NULL) {  // iter through all entry of current db
            ...
            if (rdbSaveKeyValuePair(rdb,&key,o,expire) == -1) goto werr;
            ...
        }
    }
    /* If we are storing the replication information on disk, persist
     * the script cache as well: on successful PSYNC after a restart, we need
     * to be able to process any EVALSHA inside the replication backlog the
     * master will send us. */
    if (rsi && dictSize(server.lua_scripts)) {
        di = dictGetIterator(server.lua_scripts);
        while((de = dictNext(di)) != NULL) {
            robj *body = dictGetVal(de);
            if (rdbSaveAuxField(rdb,"lua",3,body->ptr,sdslen(body->ptr)) == -1)
                goto werr;
        }
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. */
    }

    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_AFTER_RDB) == -1) goto werr;

    /* EOF opcode */
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) goto werr;
    }
    ...
    /* checksum */
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    ...
}

```

最后 在 serverCron 函数中 调用 wait 函数回收子进程， 并进行环境清理

### RDB 结构图
```C
|--  REDIS*** --|  //MAGIC NUMBER

|--  REDIS AUX -|  // 版本号 位数 时间  使用内存 RSI AOF-PREAMBLE 混合持久化等

// iter for all key space:
    |RDB_OPCODE_SELECTDB|
    |DB index|  // 第几个DB
    |RDB_OPCODE_RESIZEDB|
    |DB size|
    |expired size|
    // iter for all key entry in a key space
        |RDB_OPCODE_EXPIRETIME_MS|
        |LRU|
        |LFU|
        |KEY TYPE|
        |VALUE TYPE|
        |KEY VALUE|
    // end for iter
// end for iter

|RDB_OPCODE_EOF|

|-- CheckSum --|


/// 一些预留的 rdb 编码
#define RDB_OPCODE_MODULE_AUX 247   /* Module auxiliary data. */
#define RDB_OPCODE_IDLE       248   /* LRU idle time. LRU信息*/
#define RDB_OPCODE_FREQ       249   /* LFU frequency. LFU信息*/
#define RDB_OPCODE_AUX        250   /* RDB aux field.扩展字段 */
#define RDB_OPCODE_RESIZEDB   251   /* Hash table resize hint.提示可能需要进行哈希表的resize操作 */
#define RDB_OPCODE_EXPIRETIME_MS 252    /* Expire time in milliseconds.表示过期键，单位毫秒 */
#define RDB_OPCODE_EXPIRETIME 253       /* Old expire time in seconds.表示过期键 */
#define RDB_OPCODE_SELECTDB   254   /* DB number of the following keys.表示接下来是要选的数据库的索引id */
#define RDB_OPCODE_EOF        255   /* End of the RDB file.表示结尾 */
```
### 触发方式

- serverCron 频率与
```C
if (server.rdb_child_pid != -1 || server.aof_child_pid != -1 ||
        ldbPendingChildren())
    {...
    } else {
        for (j = 0; j < server.saveparamslen; j++) {  /*若设置了 save "",  则这里 server.saveparamslen = 0， skip , 见 config.c:loadServerConfigFromString */
        ...
        }
    }
```

- 主从复制
  
- debug reload

- shutdown elegent退出的时候， 如果配置了save, 就会进行一次rdb才退出

### 过期建处理
- 创建rdb时候会过滤掉过期的键: 只会将 redisDb->dict 内的内容备份， 而不会读取 redisDb->expires 中的键值对

- 读取时:
  - 主：只会载入未过期的；
  - 从：全部载入，因为后期主从会同步
```C
//rdbLoadRio：
 /* Read key */
        if ((key = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
        /* Read value */
        if ((val = rdbLoadObject(type,rdb)) == NULL) goto eoferr;
        //看到没，主会过滤掉。而从的话。直接载入
        if (server.masterhost == NULL && !loading_aof && expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
        }
```

### 读取RDB

pip install rdbtools

rdb --command json dump.rdb
rdb -c memory dump.rdb > dump.csv

## AOF

fflush: Ensures that data is written from the user-space buffer to the kernel-space buffer (but not necessarily to disk). It is used when you want to make sure that data in a standard I/O stream is actually passed to the operating system.

fsync: Ensures that data is written from the kernel-space buffer to the physical disk. It is used when you want to guarantee that data has been physically saved to storage, protecting against data loss in case of a crash.

- AOF是使用后台线程BIO进行的
- AOF rewrite 是利用 fork 的子进程完成的


1. AOF(append only file):以独立日志的方式记录每次写命令，重启时再重新执行AOF文件中的命令达到恢复数据的目的。AOF的主要作用是解决了数据持久化的实时性，目前已经是Redis持久化的主流方式
以redis命令请求协议格式保存的.

```C
^M----->对应的就是\r\n
$6^M
  4 SELECT^M
  5 $1^M
  6 0^M
  7 *3^M
  8 $3^M
  9 set^M
```

2. 只保存写命令(pubsub除外)
3. 支持aof重写
4. 支持RDB+AOF混合存储

### 相关配置、命令

```C
appendonly no  /* 是否打开 AOF */

appendfilename "appendonly.aof"

// The fsync() call tells the Operating System to actually write data on disk
// instead of waiting for more data in the output buffer. Some OS will really flush
// data on disk, some other OS will just try to do it ASAP.

# appendfsync always   // fsync after every write to the append only log. Slow, Safest.
appendfsync everysec  //  fsync only one time every second. Compromise.
# appendfsync no      //just let the OS flush the data when it wants. Faster


//  When the AOF fsync policy is set to always or everysec, and a background
//  saving process (a background save or AOF log background rewriting) is
//  performing a lot of I/O against the disk, in some Linux configurations
//  Redis may block too long on the fsync() call. Note that there is no fix for
//  this currently, as even performing fsync in a different thread will block
//  our synchronous write(2) call.

//  In order to mitigate this problem it's possible to use the following option
//  that will prevent fsync() from being called in the main process while a
//  BGSAVE or BGREWRITEAOF is in progress.

// This means that while another child is saving, the durability of Redis is
//  the same as "appendfsync no". In practical terms, this means that it is
//  possible to lose up to 30 seconds of log in the worst scenario (with the
//  default Linux settings).

//  If you have latency problems turn this to "yes". Otherwise leave it as
//  "no" that is the safest pick from the point of view of durability.

no-appendfsync-on-rewrite no 

// aof rewrite 条件
auto-aof-rewrite-percentage 100
auto-aof-rewrite-min-size 64mb


// # An AOF file may be found to be truncated at the end during the Redis
// # startup process, when the AOF data gets loaded back into memory.
// # This may happen when the system where Redis is running
// # crashes, especially when an ext4 filesystem is mounted without the
// # data=ordered option (however this can't happen when Redis itself
// # crashes or aborts but the operating system still works correctly).
// #
// # Redis can either exit with an error when this happens, or load as much
// # data as possible (the default now) and start if the AOF file is found
// # to be truncated at the end. The following option controls this behavior.
// #
// # If aof-load-truncated is set to yes, a truncated AOF file is loaded and
// # the Redis server starts emitting a log to inform the user of the event.
// # Otherwise if the option is set to no, the server aborts with an error
// # and refuses to start. When the option is set to no, the user requires
// # to fix the AOF file using the "redis-check-aof" utility before to restart
// # the server.
// #
// # Note that if the AOF file will be found to be corrupted in the middle
// # the server will still exit with an error. This option only applies when
// # Redis will try to read more data from the AOF file but not enough bytes
// # will be found.

aof-load-truncated yes


// 混合 aof rdb 持久化
aof-use-rdb-preamble yes

```
### 触发方式

### AOF 数据结构与 存储流程
```C
struct redisServer {
    ...
 /* AOF persistence */
    int aof_state;                  /* AOF_(ON|OFF|WAIT_REWRITE) */
    int aof_fsync;                  /* Kind of fsync() policy */
    char *aof_filename;             /* Name of the AOF file */
    int aof_no_fsync_on_rewrite;    /* Don't fsync if a rewrite is in prog. */
    int aof_rewrite_perc;           /* Rewrite AOF if % growth is > M and... */
    off_t aof_rewrite_min_size;     /* the AOF file is at least N bytes. */
    off_t aof_rewrite_base_size;    /* AOF size on latest startup or rewrite. */
    off_t aof_current_size;         /* AOF current size. */
    off_t aof_fsync_offset;         /* AOF offset which is already synced to disk. */
    int aof_rewrite_scheduled;      /* Rewrite once BGSAVE terminates. */
    pid_t aof_child_pid;            /* PID if rewriting process */
    list *aof_rewrite_buf_blocks;   /* Hold changes during an AOF rewrite. */
    sds aof_buf;      /* AOF buffer, written before entering the event loop */
    int aof_fd;       /* File descriptor of currently selected AOF file */
    int aof_selected_db; /* Currently selected DB in AOF */
    time_t aof_flush_postponed_start; /* UNIX time of postponed AOF flush */
    time_t aof_last_fsync;            /* UNIX time of last fsync() */
    time_t aof_rewrite_time_last;   /* Time used by last AOF rewrite run. */
    time_t aof_rewrite_time_start;  /* Current AOF rewrite start time. */
    int aof_lastbgrewrite_status;   /* C_OK or C_ERR */
    unsigned long aof_delayed_fsync;  /* delayed AOF fsync() counter */
    int aof_rewrite_incremental_fsync;/* fsync incrementally while aof rewriting? */
    int rdb_save_incremental_fsync;   /* fsync incrementally while rdb saving? */
    int aof_last_write_status;      /* C_OK or C_ERR */
    int aof_last_write_errno;       /* Valid if aof_last_write_status is ERR */
    int aof_load_truncated;         /* Don't stop on unexpected AOF EOF. */
    int aof_use_rdb_preamble;       /* Use RDB preamble on AOF rewrites. */
    /* AOF pipes used to communicate between parent and child during rewrite. */
    int aof_pipe_write_data_to_child;
    int aof_pipe_read_data_from_parent;
    int aof_pipe_write_ack_to_parent;
    int aof_pipe_read_ack_from_child;
    int aof_pipe_write_ack_to_child;
    int aof_pipe_read_ack_from_parent;
    int aof_stop_sending_diff;     /* If true stop sending accumulated diffs
                                      to child process. */
    sds aof_child_diff;             /* AOF diff accumulator child side. */
    ...
}

```

AOF 主要分为两步
1. 用户命令传入后， 将命令保存在 aof_buf 中
2. 通过定时任务， 将在aof_buf中的指令进行刷盘

#### step1 

expireIfNeeded -> propagateExpire -> feedAppendOnlyFile -> catAppendOnlyGenericCommand
                                        ^
processCommand -> call -> propagate --- |   

```C
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
               int flags)
{
    if (server.aof_state != AOF_OFF && flags & PROPAGATE_AOF)
        feedAppendOnlyFile(cmd,dbid,argv,argc);
    if (flags & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves,dbid,argv,argc);
}

void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    ...
        /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    if (server.aof_state == AOF_ON)
        server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));
    
    ...
    buf = catAppendOnlyGenericCommand(buf,argc,argv);
    ...
}


```

#### step2

beforeSleep                                ->       flushAppendOnlyFile(0)

serverCron (if aof_flush_postponed_start ) ->       flushAppendOnlyFile(0)               

prepareForShutdown                         ->       flushAppendOnlyFile(1)

```C
/* Write the append only file buffer on disk.
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when the
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again.
 *
 * About the 'force' argument:
 *
 * When the fsync policy is set to 'everysec' we may delay the flush if there
 * is still an fsync() going on in the background thread, since for instance
 * on Linux write(2) will be blocked by the background fsync anyway.
 * When this happens we remember that there is some aof buffer to be
 * flushed ASAP, and will try to do that in the serverCron() function.
 *
 * However if force is set to 1 we'll write regardless of the background
 * fsync. */
#define AOF_WRITE_LOG_ERROR_RATE 30 /* Seconds between errors logging. */
void flushAppendOnlyFile(int force) {
// write aof_buf to page_cache
    ...
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
        /* With this append fsync policy we do background fsyncing.
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds. */
        if (sync_in_progress) {
            if (server.aof_flush_postponed_start == 0) {
                /* No previous write postponing, remember that we are
                 * postponing the flush and return. */
                server.aof_flush_postponed_start = server.unixtime;
                return;
            } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again. */
                return;
            }
            /* Otherwise fall trough, and go write since we can't wait
             * over two seconds. */
            server.aof_delayed_fsync++;
            serverLog(LL_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }

    // synchronous write, return -1, if no bytes write successfully or number of bytes write
    nwritten = aofWrite(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
    /* We performed the write so reset the postponed flush sentinel to zero. */
    server.aof_flush_postponed_start = 0;

    if (nwritten != (ssize_t)sdslen(server.aof_buf)) {
        if (nwritten == -1) {
            // log the error
        }
        else {
            // 回滚上一次读写
            if (ftruncate(server.aof_fd, server.aof_current_size) == -1) {
                // log truncate error
            } else {
                /* If the ftruncate() succeeded we can set nwritten to
                 * -1 since there is no longer partial data into the AOF. */
                nwritten = -1;
            }
        }

         /* Handle the AOF write error. */
        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* We can't recover when the fsync policy is ALWAYS since the
             * reply for the client is already in the output buffers, and we
             * have the contract with the user that on acknowledged write data
             * is synced on disk. */
            serverLog(LL_WARNING,"Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        } else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. */
            server.aof_last_write_status = C_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). */
            if (nwritten > 0) {
                server.aof_current_size += nwritten;
                sdsrange(server.aof_buf,nwritten,-1);
            }
            return; /* We'll try again on the next call... */
        }
    } else {
        // write successfully
    }
// fsync to disk
try_fsync:
    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. */
    if (server.aof_no_fsync_on_rewrite &&
        (server.aof_child_pid != -1 || server.rdb_child_pid != -1))
            return;

    /* Perform the fsync if needed. */
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        /* redis_fsync is defined as fdatasync() for Linux in order to avoid flushing metadata. */
        redis_fsync(server.aof_fd);   // 阻塞
    } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
                server.unixtime > server.aof_last_fsync)) {
        if (!sync_in_progress) {
            aof_background_fsync(server.aof_fd);   // ************** 在子线程中处理fsync
            server.aof_fsync_offset = server.aof_current_size;
        }
        server.aof_last_fsync = server.unixtime;
    }
}
```
### AOF 格式与读取
redis支持RDB+AOF混合式，因此在读取aof文件时，会先读取前5个字节，判断是否是”REDIS”，如果是则为混合存储
```C
    /* Check if this AOF file has an RDB preamble. In that case we need to
     * load the RDB file and later continue loading the AOF tail. */
    char sig[5]; /* "REDIS" */
    if (fread(sig,1,5,fp) != 5 || memcmp(sig,"REDIS",5) != 0) {
        /* No RDB preamble, seek back at 0 offset. */
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
    } else {
        /* RDB preamble. Pass loading the RDB functions. */
        rio rdb;

        serverLog(LL_NOTICE,"Reading RDB preamble from AOF file...");
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
        rioInitWithFile(&rdb,fp);
        if (rdbLoadRio(&rdb,NULL,1) != C_OK) {
            serverLog(LL_WARNING,"Error reading the RDB preamble of the AOF file, AOF loading aborted");
            goto readerr;
        } else {
            serverLog(LL_NOTICE,"Reading the remaining AOF tail...");
        }
    }
```

- 被惰性或者定期删除后，会追加一条del指令至aof文件，

### AOF rewrite
AOF持久化是通过保存被执行的写命令来记录数据库状态的，所以随着服务器运行时间的增长，AOF文件会越来越大，这样导致使用大文件还原所需的时间也就越多。重写并不是一条条分析aof文件中的日志，而是从数据库中读取现在的值，然后用一条命令来记录键值对，代替之前记录这个键值对的多条命令。

1. 过期的键不会写入
2. 重写使用进程内数据直接生成，这样新的AOF文件只保留最终数据的写入命令。
3. 多条写命令可以合并为一个
4. 单独开辟一个子进程执行rewrite 

server.aof_child_diff

#### 触发方式


- serverCron 函数中 根据配置文件auto-aof-rewrite-min-size和auto-aof-rewrite-percentage参数确定自动触发时机。即aof_current_size>auto-aof-rewrite-minsize和 (aof_current_size-aof_base_size)/aof_base_size>=auto-aof-rewritepercentage

- 手动触发是通过bgrewriteaof命令。需要注意的是一次只能有一个子进程(无论是RDB子进程还是已有的AOF重写子进程)
```C
void bgrewriteaofCommand(client *c) {
    if (server.aof_child_pid != -1) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (server.rdb_child_pid != -1) {
        server.aof_rewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground() == C_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReply(c,shared.err);
    }
}
```

#### 重写流程

```C
/* This is how rewriting of the append only file in background works:
 *
 * 1) The user calls BGREWRITEAOF
 * 2) Redis calls this function, that forks():
 *    2a) the child rewrite the append only file in a temp file.
 *    2b) the parent accumulates differences in server.aof_rewrite_buf.
 * 3) When the child finished '2a' exists.
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.aof_rewrite_buf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 */
int rewriteAppendOnlyFileBackground(void) {
    ...
    if (aofCreatePipes() != C_OK) return C_ERR; // create pipe for communicate with child
    openChildInfoPipe();
    start = ustime();
    if ((childpid = fork()) == 0) {
        char tmpfile[256];
        /* Child */
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == C_OK) {   // aof rewrite, 这里会把当前的dict情况直接aof一遍出来
            ...
            exitFromChild(0);   // successfully rewrite
        } else {
            exitFromChild(1);
        }
    } else {
        /* Parent */
        if (childpid == -1) {
            ...
            return C_ERR;
        }
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);
        server.aof_child_pid = childpid;
        updateDictResizePolicy();
        /* We set appendseldb to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.aof_rewrite_buf will start
         * with a SELECT statement and it will be safe to merge. */
        server.aof_selected_db = -1;
        replicationScriptCacheFlush();
        return C_OK;
    }
    return C_OK; 
}

```

分为子进程 父进程两部分

##### 子进程

调用 rewriteAppendOnlyFile

```C
/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command. */
int rewriteAppendOnlyFile(char *filename) {
    rio aof;
    FILE *fp;
    char tmpfile[256];
    char byte;

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. */
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());   // 为什么用一个新的临时文件？ 方便回滚， 让写盘成为一个原子操作
    fp = fopen(tmpfile,"w");
    ...
    server.aof_child_diff = sdsempty();
    rioInitWithFile(&aof,fp);

    if (server.aof_rewrite_incremental_fsync)
        rioSetAutoSync(&aof,REDIS_AUTOSYNC_BYTES);   // 设置 autosync

    if (server.aof_use_rdb_preamble) {        // 开启rdb混合模式 则做一次rdb快照 
        int error;
        if (rdbSaveRio(&aof,&error,RDB_SAVE_AOF_PREAMBLE,NULL) == C_ERR) {
            errno = error;
            goto werr;
        }
    } else {                                 // 将当前dict进行一次aof快照
        if (rewriteAppendOnlyFileRio(&aof) == C_ERR) goto werr;   
    }

    /* Do an initial slow fsync here while the parent is still sending
     * data, in order to make the next final fsync faster. */
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;

    /* Read again a few times to get more data from the parent.
     * We can't read forever (the server may receive data from clients
     * faster than it is able to send data to the child), so we try to read
     * some more data in a loop as soon as there is a good chance more data
     * will come. If it looks like we are wasting time, we abort (this
     * happens after 20 ms without new data). */
    /*子进程fork这段时间 主进程正常提供服务，这段时间的差额由管道，通过 epoll写入*/
    int nodata = 0;
    mstime_t start = mstime();
    while(mstime()-start < 1000 && nodata < 20) {
        if (aeWait(server.aof_pipe_read_data_from_parent, AE_READABLE, 1) <= 0) // 通过 poll监听pipe的 fd， 并设置超时时间
        {
            nodata++;
            continue;
        }
        nodata = 0; /* Start counting from zero, we stop on N *contiguous*
                       timeouts. */
        aofReadDiffFromParent();
    }

    /* Ask the master to stop sending diffs. */
    if (write(server.aof_pipe_write_ack_to_parent,"!",1) != 1) goto werr;
    if (anetNonBlock(NULL,server.aof_pipe_read_ack_from_parent) != ANET_OK)
        goto werr;
    /* We read the ACK from the server using a 10 seconds timeout. Normally
     * it should reply ASAP, but just in case we lose its reply, we are sure
     * the child will eventually get terminated. */
    if (syncRead(server.aof_pipe_read_ack_from_parent,&byte,1,5000) != 1 ||
        byte != '!') goto werr;
    serverLog(LL_NOTICE,"Parent agreed to stop sending diffs. Finalizing AOF...");

    /* Read the final diff if any. */
    aofReadDiffFromParent();

    /* Write the received diff to the file. */
    serverLog(LL_NOTICE,
        "Concatenating %.2f MB of AOF diff received from parent.",
        (double) sdslen(server.aof_child_diff) / (1024*1024));
    if (rioWrite(&aof,server.aof_child_diff,sdslen(server.aof_child_diff)) == 0)
        goto werr;

    /* Make sure data will not remain on the OS's output buffers */
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;
    if (fclose(fp) == EOF) goto werr;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        serverLog(LL_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return C_ERR;
    }
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    fclose(fp);
    unlink(tmpfile);
    return C_ERR;
}
```

##### 管道的父子进程读写

```C


/*  注意顺序
    create pipe
    register epoll, 这个epoll是用来 父进程接受子进程信号， 停止发送diff的
    fork child 
*/
int aofCreatePipes(void) {
    int fds[6] = {-1, -1, -1, -1, -1, -1};
    int j;
/*Create a one-way communication channel (pipe).
   If successful, two file descriptors are stored in PIPEDES;
   bytes written on PIPEDES[1] can be read from PIPEDES[0].*/
    if (pipe(fds) == -1) goto error; /* parent -> children data. */
    if (pipe(fds+2) == -1) goto error; /* children -> parent ack. */
    if (pipe(fds+4) == -1) goto error; /* parent -> children ack. */
    /* Parent -> children data is non blocking. */
    if (anetNonBlock(NULL,fds[0]) != ANET_OK) goto error;
    if (anetNonBlock(NULL,fds[1]) != ANET_OK) goto error;
    if (aeCreateFileEvent(server.el, fds[2], AE_READABLE, aofChildPipeReadable, NULL) == AE_ERR) goto error;

    server.aof_pipe_write_data_to_child = fds[1];
    server.aof_pipe_read_data_from_parent = fds[0];
    server.aof_pipe_write_ack_to_parent = fds[3];
    server.aof_pipe_read_ack_from_child = fds[2];
    server.aof_pipe_write_ack_to_child = fds[5];
    server.aof_pipe_read_ack_from_parent = fds[4];
    server.aof_stop_sending_diff = 0;
    return C_OK;
    ...
}

/* This event handler is called when the AOF rewriting child sends us a
 * single '!' char to signal we should stop sending buffer diffs. The
 * parent sends a '!' as well to acknowledge. */
void aofChildPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask) {
    char byte;
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);

    if (read(fd,&byte,1) == 1 && byte == '!') {
        serverLog(LL_NOTICE,"AOF rewrite child asks to stop sending diffs.");
        server.aof_stop_sending_diff = 1;
        if (write(server.aof_pipe_write_ack_to_child,"!",1) != 1) {
            /* If we can't send the ack, inform the user, but don't try again
             * since in the other side the children will use a timeout if the
             * kernel can't buffer our write, or, the children was
             * terminated. */
            serverLog(LL_WARNING,"Can't send ACK to AOF child: %s",
                strerror(errno));
        }
    }
    /* Remove the handler since this can be called only one time during a
     * rewrite. */
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
}
```

##### 父进程中 

```C
rewriteAppendOnlyFileBackground {
    ...
    server.aof_rewrite_scheduled = 0;
    server.aof_rewrite_time_start = time(NULL);
    server.aof_child_pid = childpid;
    updateDictResizePolicy();
    ...
}

// 父进程处理完这些后 回到主的 aewait 循环中， 并持续写入diff

// 在进行AOF的过程中
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    /* If a background append only file rewriting is in progress we want to
     * accumulate the differences between the child DB and the current one
     * in a buffer, so that when the child process will do its work we
     * can append the differences to the new append only file. */
    if (server.aof_child_pid != -1)
        aofRewriteBufferAppend((unsigned char*)buf,sdslen(buf));

    ....
}

/* Append data to the AOF rewrite buffer, allocating new blocks if needed. */
void aofRewriteBufferAppend(unsigned char *s, unsigned long len) {
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;

    while(len) {
        /* If we already got at least an allocated block, try appending
         * at least some piece into it. */
        if (block) {
            unsigned long thislen = (block->free < len) ? block->free : len;
            if (thislen) {  /* The current block is not already full. */
                memcpy(block->buf+block->used, s, thislen);
                block->used += thislen;
                block->free -= thislen;
                s += thislen;
                len -= thislen;
            }
        }

        if (len) { /* First block to allocate, or need another block. */
            int numblocks;

            block = zmalloc(sizeof(*block));
            block->free = AOF_RW_BUF_BLOCK_SIZE;
            block->used = 0;
            listAddNodeTail(server.aof_rewrite_buf_blocks,block);

            /* Log every time we cross more 10 or 100 blocks, respectively
             * as a notice or warning. */
            numblocks = listLength(server.aof_rewrite_buf_blocks);
            if (((numblocks+1) % 10) == 0) {
                int level = ((numblocks+1) % 100) == 0 ? LL_WARNING :
                                                         LL_NOTICE;
                serverLog(level,"Background AOF buffer size: %lu MB",
                    aofRewriteBufferSize()/(1024*1024));
            }
        }
    }

    /* Install a file event to send data to the rewrite child if there is
     * not one already. */
    if (aeGetFileEvents(server.el,server.aof_pipe_write_data_to_child) == 0) {
        aeCreateFileEvent(server.el, server.aof_pipe_write_data_to_child,
            AE_WRITABLE, aofChildWriteDiffData, NULL);
    }
}


// serverCron 中 回收子进程 并处理
int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    ....

     /* Check if a background saving or AOF rewrite in progress terminated. */
    if (server.rdb_child_pid != -1 || server.aof_child_pid != -1 ||
        ldbPendingChildren())
    {
        int statloc;
        pid_t pid;

        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            ...
            } else if (pid == server.aof_child_pid) {
                backgroundRewriteDoneHandler(exitcode,bysignal);
                if (!bysignal && exitcode == 0) receiveChildInfo();
            } 
            ...
    } 
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
if (!bysignal && exitcode == 0) {
        int newfd, oldfd;
        char tmpfile[256];
        long long now = ustime();
        mstime_t latency;

        serverLog(LL_NOTICE,
            "Background AOF rewrite terminated with success");

        /* Flush the differences accumulated by the parent to the
         * rewritten AOF. */
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
            (int)server.aof_child_pid);
        newfd = open(tmpfile,O_WRONLY|O_APPEND);


        if (aofRewriteBufferWrite(newfd) == -1) { // 继续尝试将父子进程中断通信后新加入的AOF从 server.aof_rewrite_buf_blocks 中 继续追加写入
            goto cleanup;
        }
        /* The only remaining thing to do is to rename the temporary file to
         * the configured file and switch the file descriptor used to do AOF
         * writes. We don't want close(2) or rename(2) calls to block the
         * server on old file deletion.
         *
         * There are two possible scenarios:
         *
         * 1) AOF is DISABLED and this was a one time rewrite. The temporary
         * file will be renamed to the configured file. When this file already
         * exists, it will be unlinked, which may block the server.
         *
         * 2) AOF is ENABLED and the rewritten AOF will immediately start
         * receiving writes. After the temporary file is renamed to the
         * configured file, the original AOF file descriptor will be closed.
         * Since this will be the last reference to that file, closing it
         * causes the underlying file to be unlinked, which may block the
         * server.
         *
         * To mitigate the blocking effect of the unlink operation (either
         * caused by rename(2) in scenario 1, or by close(2) in scenario 2), we
         * use a background thread to take care of this. First, we
         * make scenario 1 identical to scenario 2 by opening the target file
         * when it exists. The unlink operation after the rename(2) will then
         * be executed upon calling close(2) for its descriptor. Everything to
         * guarantee atomicity for this switch has already happened by then, so
         * we don't care what the outcome or duration of that close operation
         * is, as long as the file descriptor is released again. */

        if (rename(tmpfile,server.aof_filename) == -1) {
            ...
        }
cleanup:
    ...
}
```
###
1. 优点：
- 提供更灵活的策略，来平衡性能和可靠性。
- 追加模式，容错性强，写到一半宕机或者错误，可以快速恢复
- 优先使用AOF
- 使用 info Persistence 进行监控
2. 缺点：
- 对于相同数量的数据集而言，AOF文件通常要大于RDB文件
- 恢复速度慢于rdb

# Redis 高可用

## 主从复制

- 创建从节点，复制主节点的全部数据
- 默认情况下 从节点是 read-only的 （可以设置 replica-read-only no 使得可写，但是这回导致主从数据不一致，且redis不会进行sync）
- 使用 命令 slaveof 进行创建主从关系

### 核心参数与步骤 

1. slave 执行 slaveof <ip> <port> 命令， 创建通信 socket， ping-pong会话， 身份验证
2. slave 发送PSYNC （新版本） / SYNC (旧版本)命令进行同步， 其中 PSYNC支持增量同步
3. 命令传播， master将接受执行的写命令 发送给slave, 保持同步
   1. 心跳检测， 定时向 master 发送 REPLCONF ACK <replication_offset>， 检测主从服务器网络连接状态，检测offset的缺失
   2. 如果主从服务器连接断开, slave 尝试重新连接 并发送 PSYNC指令

- 关键变量
1. replication offset  指定在replication backlog中的制偏移量， 用于比较主从间是否一致
2. replication backlog master的复制积压缓冲区
3. run ID 服务器的唯一运行ID，当重连时检测是否是同一个master来确认能否使用增量复制

```C
struct redisServer {
    ...
        /* Replication (master) */
    char replid[CONFIG_RUN_ID_SIZE+1];  /* My current replication ID. */
    long long master_repl_offset;   /* My current replication offset */
    char *repl_backlog;             /* Replication backlog for partial syncs */
    long long repl_backlog_size;    /* Backlog circular buffer size */
    long long repl_backlog_histlen; /* Backlog actual data length */
    long long repl_backlog_idx;     /* Backlog circular buffer current offset,
                                       that is the next byte will'll write to.*/
    long long repl_backlog_off;     /* Replication "master offset" of first
                                       byte in the replication backlog buffer.*/
    time_t repl_backlog_time_limit; /* Time without slaves after the backlog
                                       gets released. */
    time_t repl_no_slaves_since;    /* We have no slaves since that time.
                                       Only valid if server.slaves len is 0. */
    int repl_min_slaves_to_write;   /* Min number of slaves to write. */
    int repl_min_slaves_max_lag;    /* Max lag of <count> slaves to write. */
    int repl_diskless_sync;         /* Send RDB to slaves sockets directly. */
    int repl_diskless_sync_delay;   /* Delay to start a diskless repl BGSAVE. */
    ...
    /* Replication (slave) */
    char *masterauth;               /* AUTH with this password with master */
    char *masterhost;               /* Hostname of master */
    int masterport;                 /* Port of master */
    int repl_timeout;               /* Timeout after N seconds of master idle */
    client *master;     /* Client that is master for this slave */
    client *cached_master; /* Cached master to be reused for PSYNC. */
    int repl_syncio_timeout; /* Timeout for synchronous I/O calls */
    int repl_state;          /* Replication status if the instance is a slave */
    off_t repl_transfer_size; /* Size of RDB to read from master during sync. */
    off_t repl_transfer_read; /* Amount of RDB read from master during sync. */
    off_t repl_transfer_last_fsync_off; /* Offset when we fsync-ed last time. */
    int repl_transfer_s;     /* Slave -> Master SYNC socket */
    int repl_transfer_fd;    /* Slave -> Master SYNC temp file descriptor */
    char *repl_transfer_tmpfile; /* Slave-> master SYNC temp file name */
    time_t repl_transfer_lastio; /* Unix time of the latest read, for timeout */
    int repl_serve_stale_data; /* Serve stale data when link is down? */
    int repl_slave_ro;          /* Slave is read only? */
    /* The following two fields is where we store master PSYNC replid/offset
     * while the PSYNC is in progress. At the end we'll copy the fields into
     * the server->master client structure. */
    char master_replid[CONFIG_RUN_ID_SIZE+1];  /* Master PSYNC runid. */
    long long master_initial_offset;           /* Master PSYNC offset. */
    int repl_slave_lazy_flush;          /* Lazy FLUSHALL before loading DB? */
    /* Replication script cache. */
    dict *repl_scriptcache_dict;        /* SHA1 all slaves are aware of. */
    list *repl_scriptcache_fifo;        /* First in, first out LRU eviction. */
    unsigned int repl_scriptcache_size; /* Max number of elements. */
    /* Synchronous replication. */
    list *clients_waiting_acks;         /* Clients waiting in WAIT command. */
    int get_ack_from_slaves;            /* If true we send REPLCONF GETACK. */
    ...
}
```
### 主从关系建立
主从握手是一个建立状态机的过程
创建通信socket， ping-pong会话， 身份验证
```C
#define REPL_STATE_NONE 0 /* No active replication */
#define REPL_STATE_CONNECT 1 /* Must connect to master */
#define REPL_STATE_CONNECTING 2 /* Connecting to master */
/* --- Handshake states, must be ordered --- */
#define REPL_STATE_RECEIVE_PONG 3 /* Wait for PING reply */
#define REPL_STATE_SEND_AUTH 4 /* Send AUTH to master */
#define REPL_STATE_RECEIVE_AUTH 5 /* Wait for AUTH reply */
#define REPL_STATE_SEND_PORT 6 /* Send REPLCONF listening-port */
#define REPL_STATE_RECEIVE_PORT 7 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_IP 8 /* Send REPLCONF ip-address */
#define REPL_STATE_RECEIVE_IP 9 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_CAPA 10 /* Send REPLCONF capa */
#define REPL_STATE_RECEIVE_CAPA 11 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_PSYNC 12 /* Send PSYNC */
#define REPL_STATE_RECEIVE_PSYNC 13 /* Wait for PSYNC reply */
/* --- End of handshake states --- */
#define REPL_STATE_TRANSFER 14 /* Receiving .rdb from master */
#define REPL_STATE_CONNECTED 15 /* Connected to master */

// slave函数入口 
void replicaofCommand(client *c) {
    ...
    replicationSetMaster(char *ip, int port)；
    // 这里不进行实际的连接， 而只是设置master的信息 ip port
    ...
}

//  slave: serverCron  -> replicationCron
/* Replication cron function, called 1 time per second. */
void replicationCron(void) {
    ...
    connectWithMaster(); // 进行连接
    ...

}
```

建立好后，执行步骤2，开始发送 PSYNC, 
master 执行函数
```C

/* SYNC and PSYNC command implemenation. */
void syncCommand(client *c) {}

```
### 增量复制
```C
// 首先 在syncCommand中判断能否增量复制
void syncCommand(client *c) {

    /* Try a partial resynchronization if this is a PSYNC command.
     * If it fails, we continue with usual full resynchronization, however
     * when this happens masterTryPartialResynchronization() already
     * replied with:
     *
     * +FULLRESYNC <replid> <offset>
     *
     * So the slave knows the new replid and offset to try a PSYNC later
     * if the connection with the master is lost. */
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {
        if (masterTryPartialResynchronization(c) == C_OK) {
            server.stat_sync_partial_ok++;
            return; /* No full resync needed, return. */
        } ... 
    } else 
}

int masterTryPartialResynchronization(client *c) {

    if (strcasecmp(master_replid, server.replid) &&
        (strcasecmp(master_replid, server.replid2) ||
         psync_offset > server.second_replid_offset))
    {
        ...
        /* Run id "?" is used by slaves that want to force a full resync. */           
        goto need_full_resync;
    }

    /* We still have the data our slave is asking for? */
    if (!server.repl_backlog ||
        psync_offset < server.repl_backlog_off ||
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen))
    {
        ...
        goto need_full_resync;
    }

    /* If we reached this point, we are able to perform a partial resync:
     * 1) Set client state to make it a slave.
     * 2) Inform the client we can continue with +CONTINUE
     * 3) Send the backlog data (from the offset to the end) to the slave. */
    c->flags |= CLIENT_SLAVE;
    c->replstate = SLAVE_STATE_ONLINE;
    c->repl_ack_time = server.unixtime;
    c->repl_put_online_on_ack = 0;
    listAddNodeTail(server.slaves,c);
    ...
    psync_len = addReplyReplicationBacklog(c,psync_offset);/* Feed the slave 'c' with the replication backlog starting from the specified 'offset' up to the end of the backlog. */
    return C_OK; 

need_full_resync:
    return C_ERR;
}
```

同样这里不完成传输任务，只是将数据写入buffer

### 全量复制
```C 
// 如果不能增量写， 则必须全量复制
/* Full resynchronization. */
    server.stat_sync_full++;

    /* Setup the slave as one waiting for BGSAVE to start. The following code
     * paths will change the state if we handle the slave differently. */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    if (server.repl_disable_tcp_nodelay)
        anetDisableTcpNoDelay(NULL, c->fd); /* Non critical if it fails. */
    c->repldbfd = -1;
    c->flags |= CLIENT_SLAVE;
    listAddNodeTail(server.slaves,c);

    /* Create the replication backlog if needed. */
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL) {
        /* When we create the backlog from scratch, we always use a new
         * replication ID and clear the ID2, since there is no valid
         * past history. */
        changeReplicationId();
        clearReplicationId2();
        createReplicationBacklog();
    }

    /* CASE 1: BGSAVE is in progress, with disk target. */
    if (server.rdb_child_pid != -1 &&
        server.rdb_child_type == RDB_CHILD_TYPE_DISK)
    {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save. */
        client *slave;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) break;
        }
        /* To attach this slave, we check that it has at least all the
         * capabilities of the slave that triggered the current BGSAVE. */
        if (ln && ((c->slave_capa & slave->slave_capa) == slave->slave_capa)) {
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer. */
            copyClientOutputBuffer(c,slave);
            replicationSetupSlaveForFullResync(c,slave->psync_initial_offset);
            serverLog(LL_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences. */
            serverLog(LL_NOTICE,"Can't attach the replica to the current BGSAVE. Waiting for next BGSAVE for SYNC");
        }

    /* CASE 2: BGSAVE is in progress, with socket target. */
    } else if (server.rdb_child_pid != -1 &&
               server.rdb_child_type == RDB_CHILD_TYPE_SOCKET)
    {
        /* There is an RDB child process but it is writing directly to
         * children sockets. We need to wait for the next BGSAVE
         * in order to synchronize. */
        serverLog(LL_NOTICE,"Current BGSAVE has socket target. Waiting for next BGSAVE for SYNC");

    /* CASE 3: There is no BGSAVE is progress. */
    } else {
        if (server.repl_diskless_sync && (c->slave_capa & SLAVE_CAPA_EOF)) {
            /* Diskless replication RDB child is created inside
             * replicationCron() since we want to delay its start a
             * few seconds to wait for more slaves to arrive. */
            if (server.repl_diskless_sync_delay)
                serverLog(LL_NOTICE,"Delay next BGSAVE for diskless SYNC");
        } else {
            /* Target is disk (or the slave is not capable of supporting
             * diskless replication) and we don't have a BGSAVE in progress,
             * let's start one. */
            if (server.aof_child_pid == -1) {
                startBgsaveForReplication(c->slave_capa);
            } else {
                serverLog(LL_NOTICE,
                    "No BGSAVE in progress, but an AOF rewrite is active. "
                    "BGSAVE for replication delayed");
            }
        }
    }
    return;
```

先进行RDB，然后再进行传输， 同样这里不进行实际数据传输
而是在RDB结束阶段
serverCron -> backgroundSaveDoneHandler -> updateSlavesWaitingBgsave

### 命令传播
propagate -> replicationFeedSlaves

### 心跳检测

serverCron -> replicationSendAck

### info
info replication命令可以查看当前主从同步状态

## 哨兵模式


Redis的主从复制架构，不具备自动的高可用机制，需要人工干预进行故障转移。
而通过哨兵机制可以完成主从节点的自动切换。Redis Sentinel是一个分布式架构，包含若干个Sentinel节点和Redis数据节点，每个Sentinel节点会对数据节点和其余Sentinel节点进行监控，当发现节点不可达时，会对节点做下线标识

### 启动 与 配置

- redis-sentinel sentinel.conf
- redis-server sentinel.conf –sentinel

### 数据结构
```C
typedef struct sentinelRedisInstance {
    int flags;      /* See SRI_... defines */
    char *name;     /* Master name from the point of view of this sentinel. */
    char *runid;    /* Run ID of this instance, or unique ID if is a Sentinel.*/
    uint64_t config_epoch;  /* Configuration epoch. */
    sentinelAddr *addr; /* Master host. */
    instanceLink *link; /* Link to the instance, may be shared for Sentinels. */
    mstime_t last_pub_time;   /* Last time we sent hello via Pub/Sub. */
    mstime_t last_hello_time; /* Only used if SRI_SENTINEL is set. Last time
                                 we received a hello from this Sentinel
                                 via Pub/Sub. */
    mstime_t last_master_down_reply_time; /* Time of last reply to
                                             SENTINEL is-master-down command. */
    mstime_t s_down_since_time; /* Subjectively down since time. */
    mstime_t o_down_since_time; /* Objectively down since time. */
    mstime_t down_after_period; /* Consider it down after that period. */
    mstime_t info_refresh;  /* Time at which we received INFO output from it. */
    dict *renamed_commands;     /* Commands renamed in this instance:
                                   Sentinel will use the alternative commands
                                   mapped on this table to send things like
                                   SLAVEOF, CONFING, INFO, ... */

    /* Role and the first time we observed it.
     * This is useful in order to delay replacing what the instance reports
     * with our own configuration. We need to always wait some time in order
     * to give a chance to the leader to report the new configuration before
     * we do silly things. */
    int role_reported;
    mstime_t role_reported_time;
    mstime_t slave_conf_change_time; /* Last time slave master addr changed. */

    /* Master specific. */
    dict *sentinels;    /* Other sentinels monitoring the same master. */
    dict *slaves;       /* Slaves for this master instance. */
    unsigned int quorum;/* Number of sentinels that need to agree on failure. */
    int parallel_syncs; /* How many slaves to reconfigure at same time. */
    char *auth_pass;    /* Password to use for AUTH against master & slaves. */

    /* Slave specific. */
    mstime_t master_link_down_time; /* Slave replication link down time. */
    int slave_priority; /* Slave priority according to its INFO output. */
    mstime_t slave_reconf_sent_time; /* Time at which we sent SLAVE OF <new> */
    struct sentinelRedisInstance *master; /* Master instance if it's slave. */
    char *slave_master_host;    /* Master host as reported by INFO */
    int slave_master_port;      /* Master port as reported by INFO */
    int slave_master_link_status; /* Master link status as reported by INFO */
    unsigned long long slave_repl_offset; /* Slave replication offset. */
    /* Failover */
    char *leader;       /* If this is a master instance, this is the runid of
                           the Sentinel that should perform the failover. If
                           this is a Sentinel, this is the runid of the Sentinel
                           that this Sentinel voted as leader. */
    uint64_t leader_epoch; /* Epoch of the 'leader' field. */
    uint64_t failover_epoch; /* Epoch of the currently started failover. */
    int failover_state; /* See SENTINEL_FAILOVER_STATE_* defines. */
    struct sentinelRedisInstance *promoted_slave; /* Promoted slave instance. */
    ...

} sentinelRedisInstance;

/* Main state. */
struct sentinelState {
    char myid[CONFIG_RUN_ID_SIZE+1]; /* This sentinel ID. */
    uint64_t current_epoch;         /* Current epoch. */
    dict *masters;      /* Dictionary of master sentinelRedisInstances.
                           Key is the instance name, value is the
                           sentinelRedisInstance structure pointer. */
    ...
} sentinel;
```



### 流程

#### 建立
1. 异步建立连接
异步建立连接过程使用的函数是redis官方C客户端hredis中的函数，hredis支持多种多种适配器(ae、libev、libevent等)，使用redisAeAttach进行关联。
2. 命令连接和发布订阅连接
- 命令连接
  - 主要向所有类型的redis实例发送命令(info+ping)
- 发布-订阅连接
  - 主要是订阅与主或从建立的HELLO频道，主要是为了让sentinel自动发现，更新当前集群的状态。因此，sentinel之间不需要创建发布订阅连接。

loadServerConfig -> sentinelHandleConfiguration 读取配置

serverCron -> sentinelTimer() ->  sentinelHandleDictOfRedisInstances -> sentinelReconnectInstance  建立命令 订阅连接

#### 三类数据
1. info 获取主服务器信息
sentinel 向主服务器发送 info 命令, 得到回复

```C
# Server
...
run_id:59358a3338a4b193d20b187ace5a5a0f15ee0502
...

# Replication
role:master
connected_slaves:1
slave0:ip=127.0.0.1,port=6380,state=online,offset=106,lag=1
master_replid:a984b788d7ed997c06a129473a54f86a94de0815
master_replid2:0000000000000000000000000000000000000000
master_repl_offset:106
second_repl_offset:-1
repl_backlog_active:1
repl_backlog_size:1048576
repl_backlog_first_byte_offset:1
repl_backlog_histlen:106
```

- sentinel 据此获取主服务器信息， 以及其 “slave” 服务器， 并据此对实力结构进行更新，对从服务器也进行监听
- 对从服务器 创建命令连接 和 订阅连接

2. hello

默认情况下， sentinel以两秒一次的频率，从发布订阅连接发送命令
```C

"PUBLISH" "__sentinel__:hello" "127.0.0.1,26379,c14c9233bc96f733641ff3016023998c67ba30d8,0,mymaster,127.0.0.1,6379,0"
//                  <sentinel_ip> <sentinel_port>  <sentinel_runid>     <s_epoch>, <master_name> <m_ip> <m_port> 
```
sentinel 和一个服务器建立起订阅连接后，即通过命令连接向__sentinel__:hello 频道发送消息，又从中读取其它sentinel的消息

```C
"message" "__sentinel__:hello" "127.0.0.1,26480,c14c9233bc96f733641ff3016023998c67ba30d8,0,mymaster,127.0.0.1,6379,0"
```

如果有其他的监视相同master的sentinel在，通过这个频道相互之间即可自动发现对方，并放入 sentinelRedisInstance -> sentinel 字典中，各个sentinel之间创建**命令连接**

3. ping

默认情况下， sentinel每秒会向所有建立了命令连接的实例（master, slave, sentinel）发送ping，并通过回复判断是否在线
```C
14:21:25.724975 IP 127.0.0.1.37118 > 127.0.0.1.6379: Flags [P.], seq 308:322, ack 3434, win 3411, options [nop,nop,TS val 739585073 ecr 739584610], length 14: RESP "PING"
14:21:25.725187 IP 127.0.0.1.6379 > 127.0.0.1.37118: Flags [P.], seq 3434:3441, ack 322, win 426, options [nop,nop,TS val 739585073 ecr 739585073], length 7: RESP "PONG"
```
在 down-after-milliseconds 后无应答，则判断其为 **主观下线**， 并置为 SRI_S_DOWN

向其它观察该master的 Sentinel 发送 is-master-down-by-addr <ip> <port>, 接受其它sentinel的信息， 如
sentinel monitor mymaster 127.0.0.1 6379 2
则有包含自己已两个sentinel主观认为已经下线，则设置为 **客观下线**


通过 raft 协议 选举领头 sentinel

选择新的主节点，重新配置节点结构

主从切换

## cluster 模式

### 节点 握手
- 开启 cluster-enabled 作为节点
- cluster meet <ip> <port>

```C
typedef struct clusterNode {
    mstime_t ctime; /* Node object creation time. */
    char name[CLUSTER_NAMELEN]; /* Node name, hex string, sha1-size */
    int flags;      /* CLUSTER_NODE_... */
    uint64_t configEpoch; /* Last configEpoch observed for this node */
    unsigned char slots[CLUSTER_SLOTS/8]; /* slots handled by this node */
    int numslots;   /* Number of slots handled by this node */
    int numslaves;  /* Number of slave nodes, if this is a master */
    struct clusterNode **slaves; /* pointers to slave nodes */
    struct clusterNode *slaveof; /* pointer to the master node. Note that it
                                    may be NULL even if the node is a slave
                                    if we don't have the master node in our
                                    tables. */
    ...
    long long repl_offset;      /* Last known repl offset for this node. */
    char ip[NET_IP_STR_LEN];  /* Latest known IP address of this node */
    int port;                   /* Latest known clients port of this node */
    int cport;                  /* Latest known cluster port of this node. */
    clusterLink *link;          /* TCP/IP link with this node */
} clusterNode;

typedef struct clusterLink {
    mstime_t ctime;             /* Link creation time */
    int fd;                     /* TCP socket file descriptor */
    sds sndbuf;                 /* Packet send buffer */
    sds rcvbuf;                 /* Packet reception buffer */
    struct clusterNode *node;   /* Node related to this link if any, or NULL */
} clusterLink;

```

### slot
```C

typedef struct clusterState {
    clusterNode *myself;  /* This node */
    uint64_t currentEpoch;
    int state;            /* CLUSTER_OK, CLUSTER_FAIL, ... */
    int size;             /* Num of master nodes with at least one slot */
    dict *nodes;          /* Hash table of name -> clusterNode structures */
    dict *nodes_black_list; /* Nodes we don't re-add for a few seconds. */
    clusterNode *migrating_slots_to[CLUSTER_SLOTS];
    clusterNode *importing_slots_from[CLUSTER_SLOTS];
    clusterNode *slots[CLUSTER_SLOTS];
    uint64_t slots_keys_count[CLUSTER_SLOTS];
    rax *slots_to_keys;
    /* The following fields are used to take the slave state on elections. */
    mstime_t failover_auth_time; /* Time of previous or next election. */
    int failover_auth_count;    /* Number of votes received so far. */
    int failover_auth_sent;     /* True if we already asked for votes. */
    int failover_auth_rank;     /* This slave rank for current auth request. */
    uint64_t failover_auth_epoch; /* Epoch of the current election. */
    int cant_failover_reason;   /* Why a slave is currently not able to
                                   failover. See the CANT_FAILOVER_* macros. */
    /* Manual failover state in common. */
    mstime_t mf_end;            /* Manual failover time limit (ms unixtime).
                                   It is zero if there is no MF in progress. */
    /* Manual failover state of master. */
    clusterNode *mf_slave;      /* Slave performing the manual failover. */
    /* Manual failover state of slave. */
    long long mf_master_offset; /* Master offset the slave needs to start MF
                                   or zero if stil not received. */
    int mf_can_start;           /* If non-zero signal that the manual failover
                                   can start requesting masters vote. */
    /* The followign fields are used by masters to take state on elections. */
    uint64_t lastVoteEpoch;     /* Epoch of the last vote granted. */
    int todo_before_sleep; /* Things to do in clusterBeforeSleep(). */
    /* Messages received and sent by type. */
    long long stats_bus_messages_sent[CLUSTERMSG_TYPE_COUNT];
    long long stats_bus_messages_received[CLUSTERMSG_TYPE_COUNT];
    long long stats_pfail_nodes;    /* Number of nodes in PFAIL status,
                                       excluding nodes without address. */
} clusterState;
```
数据库切分为16384 个slot,
unsigned char slots[CLUSTER_SLOTS/8];

bit 数组， 为 1 则表示该节点负责该slot

集群中节点 互相在clusterState.node 中更新其他节点的slot

clusterNode* clusterState.slot[i] 则指向了含有slot[i]的node地址，若为NULL，则表明尚未指派

- 配置cluster的slot CLUSTER ADDSLOTS


### 集群中执行命令

- 判断键是否落在自己的slot中，如是则执行命令
- 否，返回 MOVED <slot> <ip>:<port> 错误
- 利用 cluster keyslot <key> 来计算slot
- 使用 redis-cli -c 的集群模式 实现自动转向

### 集群数据库
- 只会使用 0 号数据库
- 还使用 zskiplist 跳表来保存slot - key的关系（版本不一致这里用的radix-tree）根据 slot的值 来构建跳表

### 重新分片
redis-trib 负责执行
1. 对目标节点发送cluster setslot {slot} importing{sourceNodeId}命令，让目标节点准备导入槽的数据
2. 对源节点发送cluster setslot {slot} migrating {targetNodeId}命令，让源节点准备迁出槽的数据
3. 源节点循环执行cluster getkeysinslot {slot} {count}命令，获取count个属于槽{slot}的键
4. 在源节点上执行migrate{targetIp}{targetPort}””0{timeout}keys{keys…}命令，把获取的键通过流水线（pipeline）机制批量迁移到目标节点，批量迁移版本的migrate命令在Redis3.0.6以上版本提供，之前的migrate命令只能单个键迁移。对于大量key的场景，批量键迁移将极大降低节点之间网络IO次数.
5. 向集群内所有主节点发送cluster setslot{slot}node{targetNodeId}命令，通知槽分配给目标节点。为了保证槽节点映射变更及时传播，需要遍历发送给所有主节点更新被迁移的槽指向新节点。


### import && migrate

    clusterNode *migrating_slots_to[CLUSTER_SLOTS];
    clusterNode *importing_slots_from[CLUSTER_SLOTS];

不为空时 表明正在执行

### ASK错误
发生在该槽正在迁移时，他与move重定向的区别是，他仅仅是个提示。接受到错误的客户端根据错误提示的IP+Port，转向正在导入的槽，然后首先向目标节点发送一个ASKING命令，之后重新发送原本想要的命令。
ASKING作用就是打开客户端标识，执行ASK重定向。 从migrating_slots_to 读取

### 复制与故障转移

主从结构，多个主构成cluster
slave 复制 主 的内容

设置slave: CLUSTER REPLICATE <node_id>
struct clusterNode **slaves; /* pointers to slave nodes */
struct clusterNode *slaveof; /* pointer to the master node. Note that it
                                    may be NULL even if the node is a slave
                                    if we don't have the master node in our
                                    tables. */

slave 正在复制master 的消息会传递到整个cluster集群
各个node update 自己的 clusterState

故障检测 每个节点定期向集群中其它节点发送PING，若在规定时间内没有正确回复，则标记为 probable fail, PFAIL.

集群各个节点发送消息交换各个节点的状态信息。 半数以上的master 将某 主节点x 报告为PFAIL，则将x标记为FAIL，并发送消息

选举新的主节点。从节点向剩余的主节点要票， 同 sentinel

### 消息
- meet
- ping
- pong
// gossip

- fail
- publish


### why 16384
Normal heartbeat packets carry the full configuration of a node, that can be replaced in an idempotent way with the old in order to update an old config. This means they contain the slots configuration for a node, in raw form, that uses 2k of space with16k slots, but would use a prohibitive 8k of space using 65k slots.
At the same time it is unlikely that Redis Cluster would scale to more than 1000 mater nodes because of other design tradeoffs.