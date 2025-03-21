/*
 * Copyright (c) 2012-2013, BohuTANG <overred.shuttler at gmail dot com>
 * All rights reserved.
 * Code is licensed with GPL. See COPYING.GPL file.
 *
 * This is Bε-tree with ε=1/2, using array as LEVELs, seems like COLAS(Cache Oblivious Lookahead Arrays).
 * The main algorithm described is here: spec/small-splittable-tree.txt
 */

#include "sst.h"
#include "debug.h"
#include "xmalloc.h"

void _insertion_sort(struct sst *sst, struct sst_item *item, int len)
{
	int i, j;

	for (i = 0; i < len; i++) {
		int cmp;
		struct sst_item v = item[i];

		for (j = i - 1; j >= 0; j--) {
			cmp = strcmp(item[j].data, v.data);
			if (cmp < 0)
				break;

			else if (cmp == 0) {
				/* 
				 * Cover all of the old version when key is same in array
				 */
				if ((item[j].opt&1) &&
						((v.opt&1) == 0))
					sst->header.wasted += item[j].vlen;

				memcpy(&item[j], &v, ITEM_SIZE); 

				continue;
			}

			memcpy(&item[j + 1], &item[j], ITEM_SIZE);
		}
		memcpy(&item[j + 1], &v, ITEM_SIZE);
	}
}

int _merge_sort(struct sst *sst, struct sst_item *c,
		struct sst_item *a_new, int alen,
		struct sst_item *b_old, int blen)
{
	int i, m = 0, n = 0, k;

	for (i = 0; (m < alen) && (n < blen);) {
		int cmp;

		/* 
		 * Deduplicate data from b_old
		 */
		if (n > 0) {
			cmp = strcmp(b_old[n].data, b_old[n - 1].data);
			if (cmp == 0) {
				n++;
				continue;
			}
		}

		cmp = strcmp(a_new[m].data, b_old[n].data);
		if (cmp == 0) {
			/* 
			 * Add removed-hole to wasted
			 */
			if ((b_old[n].opt&1) && 
					((a_new[m].opt&1) == 0))
				sst->header.wasted += b_old[n].vlen;

			memcpy(&c[i++], &a_new[m], ITEM_SIZE);

			n++;
			m++;
		} else if (cmp < 0) 
			memcpy(&c[i++], &a_new[m++], ITEM_SIZE);
		else 
			memcpy(&c[i++], &b_old[n++], ITEM_SIZE);
	}

	if (m == alen) {
		for (k = n; k < blen; k++)
			memcpy(&c[i++], &b_old[k], ITEM_SIZE);
	} else if (n == blen) { 
		for (k = m; k < alen; k++)
			memcpy(&c[i++], &a_new[k], ITEM_SIZE);
	}

	return i;
}

/* 
 * Calc level's offset of the SST file 
 */
uint32_t _pos_calc(int level)
{
	int i = 0;
	uint32_t off = HEADER_SIZE;
	
	while(i < level) {
		off += (pow(LEVEL_BASE, i) * L0_SIZE);
		i++;
	}

	return off;
}

void _update_header(struct sst *sst)
{
	int res;

	res = pwrite(sst->fd, &sst->header, HEADER_SIZE, 0);
	if (res == -1)
		return;
}

uint32_t _level_max(int level, int gap)
{
	return (uint32_t)(pow(LEVEL_BASE, level) * L0_SIZE / ITEM_SIZE - gap);
}

void sst_dump(struct sst *sst) {
	int i;

	__DEBUG("**%06d.SST dump:", 
			sst->fd);

	for(i = 0; i< (int)MAX_LEVEL; i++) {
		printf("\t\t-L#%d---count#%d, max-count:%d\n",
				i,
				sst->header.count[i],
				(int)_level_max(i, 0));
	}
	printf("\n");
}

struct sst_item * read_one_level(struct sst *sst, int level, uint32_t readc, int issort)
{
	int res;
	int c = sst->header.count[level];
	struct sst_item *L = xcalloc(readc + 1, ITEM_SIZE);

	if (c > 0) {
		res = pread(sst->fd, L, readc * ITEM_SIZE, _pos_calc(level) + (c - readc) * ITEM_SIZE);
		if (res == -1)
			__PANIC("read klen error");

		if (level == 0 && issort)
			_insertion_sort(sst, L, readc);
	}

	return L;
}

void write_one_level(struct sst *sst, struct sst_item *L, uint32_t count, int level)
{
	int res;

	block_build(sst->blk, L, count, level);
	res = pwrite(sst->fd, L, count * ITEM_SIZE, _pos_calc(level));
	if (res == -1)
		__PANIC("write to one level....");
}

void  _merge_to_next(struct sst *sst, int level) 
{
	int nxt_level = level + 1;
	uint32_t c1 = sst->header.count[level];
	uint32_t c2 = sst->header.count[nxt_level];
	uint32_t lmerge_c = c1 + c2;
	struct sst_item *L = read_one_level(sst, level, c1, 1);
	struct sst_item *L_nxt = read_one_level(sst, nxt_level, c2, 1);
	struct sst_item *L_merge = xcalloc(lmerge_c + 1, ITEM_SIZE);

	lmerge_c = _merge_sort(sst, L_merge, L, c1, L_nxt, c2);
	write_one_level(sst, L_merge, lmerge_c, nxt_level);

	sst->header.count[level] = 0;
	sst->header.count[nxt_level] = lmerge_c;

	/* 
	 * Update full flag when there is enough size
	 */
	sst->header.full[level] = 0;
	if (lmerge_c >= _level_max(nxt_level, 3))
		sst->header.full[nxt_level] = 1;

	_update_header(sst);

	xfree(L);
	xfree(L_nxt);
	xfree(L_merge);

	sst->stats->STATS_LEVEL_MERGES++;
} 

void _check_merge(struct sst *sst)
{
	int i;
	int full = 0;

	for (i = MAX_LEVEL - 2; i >= 0; i--) {
		if (sst->header.full[i]) {
			/* 
			 * Level-i and Level-(i+1) all is full
			 */
			if (sst->header.full[i + 1] == 0) {
				int c = sst->header.count[i];
				int nxt_c = sst->header.count[i + 1];
				int nxt_max = _level_max(i + 1, 3);
				int delta = nxt_max - (c + nxt_c);

				/* 
				 * Merge full level to next level
				 */
				if (delta >= 0)
					_merge_to_next(sst, i);
				else 
					sst->header.full[i + 1] = 1;
			}
		}
	}

	for (i = MAX_LEVEL - 1; i >= 0; i--) {
		if (sst->header.full[i]) 
			full++;
	}

	if (full >= (MAX_LEVEL - 1))
		sst->willfull = 1;
}

void _build_block(struct sst *sst)
{
	int i;

	for (i = 1; i < MAX_LEVEL; i++) {
		int c;
		struct sst_item *L;

		c = sst->header.count[i];
		L = read_one_level(sst, i, c, 1);
		if (c > 0)
			block_build(sst->blk, L, c, i); 
		xfree(L);
	}
}

#define BLOCK0_COUNT ((L0_SIZE/ITEM_SIZE)/BLOCK_GAP + 1)
struct sst *sst_new(const char *file, struct stats *stats)
{
	int res;
	struct sst *sst = xcalloc(1, sizeof(struct sst));

	sst->oneblk = xcalloc(BLOCK_GAP, ITEM_SIZE);
	sst->blk= block_new(BLOCK0_COUNT);

	sst->fd = n_open(file, N_OPEN_FLAGS, 0644);
	if (sst->fd > 0) {
		res = pread(sst->fd, &sst->header, HEADER_SIZE, 0);
		if (res == -1)
			goto ERR;

		_build_block(sst);
	} else
		sst->fd = n_open(file, N_CREAT_FLAGS, 0644);

	sst->stats = stats;
	sst->bf = bloom_new(sst->header.bitset);


	sst->lock = xmalloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(sst->lock, NULL);

	return sst;

ERR:
	if (sst)
		xfree(sst);

	return NULL;
}

int sst_add(struct sst *sst, struct sst_item *item)
{
	int cmp;
	int res;
	int pos;

	/* 
	 * Bloom filter
	 */
	if (item->opt & 1)
		bloom_add(sst->bf, item->data);

	int klen = strlen(item->data);

	pos = HEADER_SIZE + sst->header.count[0] * ITEM_SIZE;

	/* 
	 * Swap max key 
	 */
	cmp = strcmp(item->data, sst->header.max_key);
	if (cmp > 0) { 
		memset(sst->header.max_key, 0, NESSDB_MAX_KEY_SIZE);
		memcpy(sst->header.max_key, item->data, klen);
	}

	res = pwrite(sst->fd, item, ITEM_SIZE, pos);
	if (res == -1)
		goto ERR;

	sst->header.count[0]++;
	_update_header(sst);

	/* 
	 * If L0 is full, to check
	 */
	if (sst->header.count[0] >= _level_max(0, 1)) {
		sst->header.full[0] = 1;
		_check_merge(sst);
	}
	
	return 1;

ERR:
	return 0;
}

void sst_truncate(struct sst *sst)
{
	memset(&sst->header, 0, HEADER_SIZE);
	sst->willfull = 0;
}

struct sst_item *sst_in_one(struct sst *sst, int *c) 
{
	int i;
	int cur_lc;
	int pre_lc = 0;
	struct sst_item *L = NULL; 

	for (i = 0; i < MAX_LEVEL; i++) {
		cur_lc = sst->header.count[i];
		if (cur_lc > 0) {
			if (i == 0) {
				L = read_one_level(sst, i, cur_lc, 1);
				pre_lc += cur_lc;
			} else {
				struct sst_item *pre = L;
				struct sst_item *cur = read_one_level(sst, i, cur_lc, 1);

				L = xcalloc(cur_lc + pre_lc + 1, ITEM_SIZE);
				pre_lc = _merge_sort(sst, L, cur, cur_lc, pre, pre_lc);

				xfree(pre);
				xfree(cur);
			}
		}
	}
	*c = pre_lc;
	sst->stats->STATS_SST_MERGEONE++;

	return L;
}

#define SST_BLOCK_SIZE (BLOCK_GAP * ITEM_SIZE)
int sst_get(struct sst *sst, struct slice *sk, struct ol_pair *pair)
{
	int cmp;
	int i = 0;
	uint32_t c = sst->header.count[i];
	struct sst_item *L = read_one_level(sst, 0, c, 0);

	/* 
	 * Linear Search in level 0
	 */
	for (i = c - 1; i >= 0; i--) {
		cmp = strcmp(sk->data, L[i].data);
		if (cmp == 0) {
			if (L[i].opt&1) {
				pair->offset = L[i].offset;
				pair->vlen = L[i].vlen;
			}
			xfree(L);

			goto RET;
		}
	}
	xfree(L);

	for (i = 1; i < MAX_LEVEL; i++) {
		int k;
		int res;
		int blk_idx;

		if (sst->header.count[i] == 0)
			continue;

		blk_idx = block_search(sst->blk, sk, i);
		if (blk_idx < 0)
			continue;

		res = pread(sst->fd, sst->oneblk, SST_BLOCK_SIZE, _pos_calc(i) + blk_idx * SST_BLOCK_SIZE);
		if (res == -1)
			goto ERR;

		for (k = 0; k < BLOCK_GAP; k++) {
			cmp = strncmp(sk->data, sst->oneblk[k].data, sk->len);
			if (cmp == 0) {
				if (sst->oneblk[k].opt&1) {
					pair->offset = sst->oneblk[k].offset;
					pair->vlen = sst->oneblk[k].vlen;
				}
				goto RET;
			}
		}
	}

RET:
	return 1;
ERR:
	return 0;
}

void sst_free(struct sst *sst)
{
	pthread_mutex_unlock(sst->lock);
	pthread_mutex_destroy(sst->lock);
	xfree(sst->lock);

	if (sst->fd > 0)
		close(sst->fd);

	bloom_free(sst->bf);
	block_free(sst->blk);
	xfree(sst->oneblk);
	xfree(sst);
}
