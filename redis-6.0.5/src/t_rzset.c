//
// Created by user on 18-10-6.
//

#include "server.h"
#include "CRDT.h"
#include "RWFramework.h"

#ifdef CRDT_OVERHEAD
static long rze_count=0;
#endif

#define RW_RPQ_TABLE_SUFFIX "_rzets_"
#define LOOKUP(e) (listLength((e)->aset) != 0 && listLength((e)->rset) == 0)
#define SCORE(e) ((e)->value->x + (e)->value->inc)
#define RZESIZE(e) (sizeof(rze) + 3 * sizeof(list) + VC_SIZE(e->current) \
                    + listLength((e)->aset) * (sizeof(rz_ase) + sizeof(listNode) + VC_SIZE(e->current)) \
                    + listLength((e)->rset) * (VC_SIZE(e->current) + sizeof(listNode))\
                    + listLength((e)->ops) * (sizeof(rz_cmd) + sizeof(listNode) + VC_SIZE(e->current)))

typedef struct rzset_aset_element
{
    vc *t;
    double x;
    double inc;
} rz_ase;

sds rz_aseToSds(rz_ase *a)
{
    sds vc_s = VCToSds(a->t);
    sds s = sdscatprintf(sdsempty(), "%f %f %s",
                         a->x, a->inc, vc_s);
    sdsfree(vc_s);
    return s;
}

typedef struct RW_RPQ_element
{
    vc *current;
    rz_ase *value;
    list *aset;
    list *rset;
    list *ops;
} rze;

rze *rzeNew()
{
    rze *e = zmalloc(sizeof(rze));
    e->current = l_newVC;
    e->value = NULL;
    e->aset = listCreate();
    e->rset = listCreate();
    e->ops = listCreate();
    return e;
}

rze *rzeHTGet(redisDb *db, robj *tname, robj *key, int create)
{
    robj *ht = getInnerHT(db, tname, RW_RPQ_TABLE_SUFFIX, create);
    if (ht == NULL)return NULL;
    robj *value = hashTypeGetValueObject(ht, key->ptr);
    rze *e;
    if (value == NULL)
    {
        if (!create)return NULL;
        e = rzeNew();
        hashTypeSet(ht, key->ptr, sdsnewlen(&e, sizeof(rze *)), HASH_SET_TAKE_VALUE);
#ifdef CRDT_OVERHEAD
        inc_ovhd_count(cur_db, cur_tname, SUF_RZETOTAL, 1);
#endif
    }
    else
    {
        e = *(rze **) (value->ptr);
        decrRefCount(value);
    }
    return e;
}

enum rz_cmd_type
{
    RZADD, RZINCBY, RZREM
};
typedef struct rzset_unready_command
{
    enum rz_cmd_type type;
    double value;
    vc *t;
} rz_cmd;

sds rz_cmdToSds(rz_cmd *cmd)
{
    static char *tp[] = {"RZADD", "RZINCBY", "RZREM"};
    sds vc_s = VCToSds(cmd->t);
    sds s = sdscatprintf(sdsempty(), "%s %f %s",
                         tp[cmd->type], cmd->value, vc_s);
    sdsfree(vc_s);
    return s;
}

// get the ownership of t
rz_cmd *rz_cmdNew(enum rz_cmd_type type, double value, vc *t)
{
    rz_cmd *cmd = zmalloc(sizeof(rz_cmd));
    cmd->type = type;
    cmd->value = value;
    cmd->t = t;
    return cmd;
}

void rz_cmdDelete(rz_cmd *cmd)
{
    deleteVC(cmd->t);
    zfree(cmd);
}

// doesn't free t, doesn't own t
static void insertFunc(rze *e, redisDb *db, robj *tname, robj *key, double value, vc *t)
{
    rz_ase *a = zmalloc(sizeof(rz_ase));
    a->t = duplicateVC(t);
    a->x = value;
    a->inc = 0;
    listAddNodeTail(e->aset, a);

    listNode *ln;
    listIter li;
    listRewind(e->rset, &li);
    while ((ln = listNext(&li)))
    {
        vc *r = ln->value;
        if (compareVC(r, t) == CLOCK_LESS)
        {
            listDelNode(e->rset, ln);
            deleteVC(r);
        }
    }

    if (e->value == NULL || e->value->t->id < t->id)
        e->value = a;
    if (LOOKUP(e))
    {
        robj *zset = getZsetOrCreate(db, tname, key);
        int flags = ZADD_NONE;
        zsetAdd(zset, SCORE(e), key->ptr, &flags, NULL);
    }
    updateVC(e->current, t);
    server.dirty++;
}

static void increaseFunc(rze *e, redisDb *db, robj *tname, robj *key, double value, vc *t)
{
    listNode *ln;
    listIter li;
    listRewind(e->aset, &li);
    while ((ln = listNext(&li)))
    {
        rz_ase *a = ln->value;
        if (compareVC(a->t, t) == CLOCK_LESS)
            a->inc += value;
    }

    if (LOOKUP(e) && compareVC(e->value->t, t) == CLOCK_LESS)
    {
        robj *zset = getZsetOrCreate(db, tname, key);
        int flags = ZADD_NONE;
        zsetAdd(zset, SCORE(e), key->ptr, &flags, NULL);
    }
    updateVC(e->current, t);
    server.dirty++;
}

static void removeFunc(rze *e, redisDb *db, robj *tname, robj *key, vc *t)
{
    vc *r = duplicateVC(t);
    listAddNodeTail(e->rset, r);

    listNode *ln;
    listIter li;
    listRewind(e->aset, &li);
    while ((ln = listNext(&li)))
    {
        rz_ase *a = ln->value;
        if (compareVC(a->t, t) == CLOCK_LESS)
        {
            listDelNode(e->aset, ln);
            if (e->value == a)
                e->value = NULL;
            deleteVC(a->t);
            zfree(a);
        }
    }

    if (e->value == NULL)
    {
        listRewind(e->aset, &li);
        while ((ln = listNext(&li)))
        {
            rz_ase *a = ln->value;
            if (e->value == NULL || e->value->t->id < a->t->id)
                e->value = a;
        }
    }

    if (!LOOKUP(e))
    {
        robj *zset = getZsetOrCreate(db, tname, key);
        zsetDel(zset, key->ptr);
    }
    updateVC(e->current, t);
    server.dirty++;
}

static void notifyLoop(rze *e, redisDb *db, robj *tname, robj *key)
{
    int changed;
    do
    {
        changed = 0;
        listNode *ln;
        listIter li;
        listRewind(e->ops, &li);
        while ((ln = listNext(&li)))
        {
            rz_cmd *cmd = ln->value;
            if (causally_ready(e->current, cmd->t))
            {
                changed = 1;
                switch (cmd->type)
                {
                    case RZADD:
                        insertFunc(e, db, tname, key, cmd->value, cmd->t);
                        break;
                    case RZINCBY:
                        increaseFunc(e, db, tname, key, cmd->value, cmd->t);
                        break;
                    case RZREM:
                        removeFunc(e, db, tname, key, cmd->t);
                        break;
                    default:
                        serverPanic("unknown rzset cmd type.");
                }
                listDelNode(e->ops, ln);
                rz_cmdDelete(cmd);
                break;
            }
        }
    } while (changed);
}

/*
 * not full causal delivery. add/inc wait all previous rmv to be ready.


int readyCheck(rze *e, vc *t)
{
    int *current = e->current->vector;
    int *next = t->vector;
    int equal = 1;
    for (int i = 0; i < t->size; ++i)
    {
        if (current[i] > next[i])
            return 1;
        if (current[i] < next[i])
            equal = 0;
    }
    return equal;
}


// no memory free
void insertFunc(rze *e, redisDb *db, robj *tname, robj *element, double value, vc *t)
{
    if (!insertCheck(e, t))return;
    e->aid = t->id;
    e->innate = value;
    robj *zset = getZsetOrCreate(db, tname, element);
    int flags = ZADD_NONE;
    zsetAdd(zset, SCORE(e), element->ptr, &flags, NULL);
    server.dirty++;
}

void increaseFunc(rze *e, redisDb *db, robj *tname, robj *element, double value, vc *t)
{
    if (!increaseCheck(e, t))return;
    e->acquired += value;
    robj *zset = getZsetOrCreate(db, tname, element);
    int flags = ZADD_NONE;
    zsetAdd(zset, SCORE(e), element->ptr, &flags, NULL);
    server.dirty++;
}


void notifyLoop(rze *e, redisDb *db)
{
    listNode *ln;
    listIter li;
    listRewind(e->ops, &li);
    while ((ln = listNext(&li)))
    {
        ucmd *cmd = ln->value;
        if (readyCheck(e, cmd->t))
        {
            switch (cmd->type)
            {
                case RZADD:
                    insertFunc(e, db, cmd->tname, cmd->element, cmd->value, cmd->t);
                    break;
                case RZINCBY:
                    increaseFunc(e, db, cmd->tname, cmd->element, cmd->value, cmd->t);
                    break;
                default:
                    serverPanic("unknown rzset cmd type.");
            }
            listDelNode(e->ops, ln);
            ucmdDelete(cmd);
        }
    }
}


            // in effect of addCommand function:
            if (readyCheck(e, t))
            {
                insertFunc(e, c->db, c->rargv[1], c->rargv[2], v, t);
                deleteVC(t);
            }
            else
            {
                ucmd *cmd = ucmdNew(RZADD, c->rargv[1], c->rargv[2], v, t);
                listAddNodeTail(e->ops, cmd);
            }

            // in effect of incCommand function:
            if (readyCheck(e, t))
            {
                increaseFunc(e, c->db, c->rargv[1], c->rargv[2], v, t);
                deleteVC(t);
            }
            else
            {
                ucmd *cmd = ucmdNew(RZINCBY, c->rargv[1], c->rargv[2], v, t);
                listAddNodeTail(e->ops, cmd);
            }

            // in effect of remCommand function:
            if (removeCheck(e, t))
            {
                updateVC(e->current, t);
                e->aid = -1;
                e->acquired = 0;
                e->innate = 0;
                robj *zset = getZsetOrCreate(c->db, c->rargv[1], c->rargv[2]);
                zsetDel(zset, c->rargv[2]->ptr);
                server.dirty++;
                notifyLoop(e, c->db);
            }


*/



void rzaddCommand(client *c)
{
#ifdef CRDT_OVERHEAD
    PRE_SET;
#endif
    CRDT_BEGIN
        CRDT_PREPARE
            CHECK_ARGC_AND_CONTAINER_TYPE(OBJ_ZSET, 4);
            CHECK_ARG_TYPE_DOUBLE(c->argv[3]);
            rze *e = rzeHTGet(c->db, c->argv[1], c->argv[2], 1);
            if (LOOKUP(e))
            {
                addReply(c, shared.ele_exist);
                return;
            }
            RARGV_ADD_SDS(nowVC(e->current));
        CRDT_EFFECT
            double v;
            getDoubleFromObject(c->rargv[3], &v);
            vc *t = SdsToVC(c->rargv[4]->ptr);
            rze *e = rzeHTGet(c->db, c->rargv[1], c->rargv[2], 1);
            if (causally_ready(e->current, t))
            {
                insertFunc(e, c->db, c->rargv[1], c->rargv[2], v, t);
                deleteVC(t);
                notifyLoop(e, c->db, c->rargv[1], c->rargv[2]);
            }
            else
            {
                rz_cmd *cmd = rz_cmdNew(RZADD, v, t);
                listAddNodeTail(e->ops, cmd);
            }
    CRDT_END
}

void rzincrbyCommand(client *c)
{
#ifdef CRDT_OVERHEAD
    PRE_SET;
#endif
    CRDT_BEGIN
        CRDT_PREPARE
            CHECK_ARGC_AND_CONTAINER_TYPE(OBJ_ZSET, 4);
            CHECK_ARG_TYPE_DOUBLE(c->argv[3]);
            rze *e = rzeHTGet(c->db, c->argv[1], c->argv[2], 0);
            if (e == NULL || !LOOKUP(e))
            {
                addReply(c, shared.ele_nexist);
                return;
            }
            RARGV_ADD_SDS(nowVC(e->current));
        CRDT_EFFECT
            double v;
            getDoubleFromObject(c->rargv[3], &v);
            vc *t = SdsToVC(c->rargv[4]->ptr);
            rze *e = rzeHTGet(c->db, c->rargv[1], c->rargv[2], 1);
            if (causally_ready(e->current, t))
            {
                increaseFunc(e, c->db, c->rargv[1], c->rargv[2], v, t);
                deleteVC(t);
                notifyLoop(e, c->db, c->rargv[1], c->rargv[2]);
            }
            else
            {
                rz_cmd *cmd = rz_cmdNew(RZINCBY, v, t);
                listAddNodeTail(e->ops, cmd);
            }
    CRDT_END
}

void rzremCommand(client *c)
{
#ifdef CRDT_OVERHEAD
    PRE_SET;
#endif
    CRDT_BEGIN
        CRDT_PREPARE
            CHECK_ARGC_AND_CONTAINER_TYPE(OBJ_ZSET, 3);
            rze *e = rzeHTGet(c->db, c->argv[1], c->argv[2], 0);
            if (e == NULL || !LOOKUP(e))
            {
                addReply(c, shared.ele_nexist);
                return;
            }
            RARGV_ADD_SDS(nowVC(e->current));
        CRDT_EFFECT
            vc *t = SdsToVC(c->rargv[3]->ptr);
            rze *e = rzeHTGet(c->db, c->rargv[1], c->rargv[2], 1);
            if (causally_ready(e->current, t))
            {
                removeFunc(e, c->db, c->rargv[1], c->rargv[2], t);
                deleteVC(t);
                notifyLoop(e, c->db, c->rargv[1], c->rargv[2]);
            }
            else
            {
                rz_cmd *cmd = rz_cmdNew(RZREM, 0, t);
                listAddNodeTail(e->ops, cmd);
            }
    CRDT_END
}

void rzscoreCommand(client *c)
{
    robj *key = c->argv[1];
    robj *zobj;
    double score;

    if ((zobj = lookupKeyReadOrReply(c, key, shared.null[c->resp])) == NULL ||
        checkType(c, zobj, OBJ_ZSET))
        return;

    if (zsetScore(zobj, c->argv[2]->ptr, &score) == C_ERR)
    {
        addReply(c, shared.null[c->resp]);
    }
    else
    {
        addReplyDouble(c, score);
    }
}

void rzmaxCommand(client *c)
{
    robj *zobj;
    if ((zobj = lookupKeyReadOrReply(c, c->argv[1], shared.emptyarray)) == NULL
        || checkType(c, zobj, OBJ_ZSET))
        return;
    if (zsetLength(zobj) == 0)
    {
        addReply(c, shared.emptyarray);

#ifdef CRDT_LOG
        CRDT_log("%s %s, NONE",
                 (char *)(c->argv[0]->ptr),
                 (char *)(c->argv[1]->ptr));
#endif

        return;
    }
    addReplyArrayLen(c, 2);
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST)
    {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        eptr = ziplistIndex(zl, -2);
        sptr = ziplistNext(zl, eptr);

        serverAssertWithInfo(c, zobj, eptr != NULL && sptr != NULL);
        serverAssertWithInfo(c, zobj, ziplistGet(eptr, &vstr, &vlen, &vlong));
        if (vstr == NULL)
            addReplyBulkLongLong(c, vlong);
        else
            addReplyBulkCBuffer(c, vstr, vlen);
        addReplyDouble(c, zzlGetScore(sptr));

#ifdef CRDT_LOG
        if (vstr == NULL)
            CRDT_log("%s %s, %ld: %f",
                     (char *)(c->argv[0]->ptr),
                     (char *)(c->argv[1]->ptr),
                     (long)vlong, zzlGetScore(sptr));
        else
        {
            char *temp = zmalloc(sizeof(char) * (vlen + 1));
            for (unsigned int i = 0; i < vlen; ++i)
                temp[i] = vstr[i];
            temp[vlen] = '\0';
            CRDT_log("%s %s, %s: %f",
                     (char *)(c->argv[0]->ptr),
                     (char *)(c->argv[1]->ptr),
                     temp, zzlGetScore(sptr));
            zfree(temp);
        }
#endif

    }
    else if (zobj->encoding == OBJ_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln = zsl->tail;
        serverAssertWithInfo(c, zobj, ln != NULL);
        sds ele = ln->ele;
        addReplyBulkCBuffer(c, ele, sdslen(ele));
        addReplyDouble(c, ln->score);
 
#ifdef CRDT_LOG
        CRDT_log("%s %s, %s: %f",
                 (char *)(c->argv[0]->ptr),
                 (char *)(c->argv[1]->ptr),
                 ele, ln->score);
#endif

    }
    else
    {
        serverPanic("Unknown sorted set encoding");
    }
}

#ifdef CRDT_ELE_STATUS
void rzestatusCommand(client *c)
{
    rze *e = rzeHTGet(c->db, c->argv[1], c->argv[2], 0);
    if (e == NULL)
    {
        addReply(c, shared.emptyarray);
        return;
    }

    long len = 7 + listLength(e->aset) + listLength(e->rset) + listLength(e->ops);
    addReplyArrayLen(c, len);

    addReplyBulkSds(c, sdsnew("current:"));
    addReplyBulkSds(c, VCToSds(e->current));

    addReplyBulkSds(c, sdscatprintf(sdsempty(), "value:"));
    addReplyBulkSds(c, rz_aseToSds(e->value));

    listNode *ln;
    listIter li;

    addReplyBulkSds(c, sdsnew("Add Set:"));
    listRewind(e->aset, &li);
    while ((ln = listNext(&li)))
    {
        rz_ase *a = ln->value;
        addReplyBulkSds(c, rz_aseToSds(a));
    }

    addReplyBulkSds(c, sdsnew("Remove Set:"));
    listRewind(e->rset, &li);
    while ((ln = listNext(&li)))
    {
        vc *a = ln->value;
        addReplyBulkSds(c, VCToSds(a));
    }

    addReplyBulkSds(c, sdsnew("unready commands:"));
    listRewind(e->ops, &li);
    while ((ln = listNext(&li)))
    {
        rz_cmd *cmd = ln->value;
        addReplyBulkSds(c, rz_cmdToSds(cmd));
    }
}
#endif

#ifdef CRDT_OPCOUNT
void rzopcountCommand(client *c)
{
    addReplyLongLong(c, get_op_count());
}
#endif

/* Actually the hash set used here to store rze structures is not necessary.
 * We can store rze in the zset, for it's whether ziplist or dict+skiplist.
 * We use the hash set here for fast implementing our CRDT Algorithms.
 * We may optimize our implementation by not using the hash set and using
 * zset's own dict instead in the future.
 * As for metadata overhead calculation, we here do it as if we have done
 * such optimization. The commented area is the overhead if we take the
 * hash set into account.
 *
 * optimized:
 * zset:
 * key -> score(double)
 * --->
 * key -> pointer that point to metadata (rze*)
 *
 * the metadata contains score information
 * overall the metadata overhead is size used by rze
 * */
#ifdef CRDT_OVERHEAD

void rzoverheadCommand(client *c)
{
    PRE_SET;
    long long size = get_ovhd_count(cur_db, cur_tname, SUF_RZETOTAL) *
                     (sizeof(rze) + sizeof(vc) + server.p2p_count * sizeof(int));
    addReplyLongLong(c, size);
}

#else

void rzoverheadCommand(client *c)
{
    robj *htname = createObject(OBJ_STRING, sdscat(sdsdup(c->argv[1]->ptr), RW_RPQ_TABLE_SUFFIX));
    robj *ht = lookupKeyRead(c->db, htname);
    long long size = 0;

    /*
     * The overhead for database to store the hash set information.
     * sds temp = sdsdup(htname->ptr);
     * size += sizeof(dictEntry) + sizeof(robj) + sdsAllocSize(temp);
     * sdsfree(temp);
     */

    decrRefCount(htname);
    if (ht == NULL)
    {
        addReplyLongLong(c, 0);
        return;
    }

    hashTypeIterator *hi = hashTypeInitIterator(ht);
    while (hashTypeNext(hi) != C_ERR)
    {
        sds value = hashTypeCurrentObjectNewSds(hi, OBJ_HASH_VALUE);
        rze *e = *(rze **) value;
        //size += rzeSize(e);
        size += RZESIZE(e);
        sdsfree(value);
    }
    hashTypeReleaseIterator(hi);
    addReplyLongLong(c, size);
    /*
    if (ht->encoding == OBJ_ENCODING_ZIPLIST)
    {
        // Not implemented. We show the overhead calculation method:
        // size += (size of the ziplist structure itself) + (size of keys and values);
        // Iterate the ziplist to get each rze* e;
        // size += rzeSize(e);
    }
    else if (ht->encoding == OBJ_ENCODING_HT)
    {
        dict *d = ht->ptr;
        size += sizeof(dict) + sizeof(dictType) + (d->ht[0].size + d->ht[1].size) * sizeof(dictEntry *)
                + (d->ht[0].used + d->ht[1].used) * sizeof(dictEntry);

        dictIterator *di = dictGetIterator(d);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL)
        {
            sds key = dictGetKey(de);
            sds value = dictGetVal(de);
            size += sdsAllocSize(key) + sdsAllocSize(value);
            rze *e = *(rze **) value;
            size += rzeSize(e);
        }
        dictReleaseIterator(di);
    }
    else
    {
        serverPanic("Unknown hash encoding");
    }
    */
}

#endif