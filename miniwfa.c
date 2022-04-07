#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "miniwfa.h"
#include "kalloc.h"

/*
 * Default setting
 */
void mwf_opt_init(mwf_opt_t *opt)
{
	memset(opt, 0, sizeof(*opt));
	opt->x  = 2;
	opt->o1 = 4, opt->e1 = 2;
	opt->o2 = 24, opt->e2 = 1;
}

/*
 * Structs and simple functions for traceback
 */
typedef struct {
	int32_t lo, hi;
	uint8_t *x;
} wf_tb1_t;

typedef struct {
	int32_t m, n;
	wf_tb1_t *a;
} wf_tb_t;

static wf_tb1_t *wf_tb_add(void *km, wf_tb_t *tb, int32_t lo, int32_t hi)
{
	wf_tb1_t *p;
	if (tb->n == tb->m) {
		tb->m += (tb->m>>1) + 4;
		tb->a = Krealloc(km, wf_tb1_t, tb->a, tb->m);
	}
	p = &tb->a[tb->n++];
	p->lo = lo, p->hi = hi;
	p->x = Kcalloc(km, uint8_t, hi - lo + 1);
	return p;
}

typedef struct {
	int32_t m, n;
	uint32_t *cigar;
} wf_cigar_t;

static void wf_cigar_push1(void *km, wf_cigar_t *c, int32_t op, int32_t len)
{
	if (c->n && op == (c->cigar[c->n-1]&0xf)) {
		c->cigar[c->n-1] += len<<4;
	} else {
		if (c->n == c->m) {
			c->m = c->m + (c->m>>1) + 4;
			c->cigar = Krealloc(km, uint32_t, c->cigar, c->m);
		}
		c->cigar[c->n++] = len<<4 | op;
	}
}

/*
 * The stripe data structure
 */
#define WF_NEG_INF (-0x40000000)

typedef struct {
	int32_t lo, hi;
	int32_t *mem, *H, *E1, *E2, *F1, *F2;
} wf_slice_t;

typedef struct {
	int32_t s, top, n, max_pen, lo, hi;
	wf_slice_t *a;
} wf_stripe_t;

void wf_stripe_add(void *km, wf_stripe_t *wf, int32_t lo, int32_t hi)
{
	int32_t i, n, m1 = wf->max_pen + 1, m2 = m1 * 2;
	wf_slice_t *f;
	++wf->s;
	++wf->top;
	if (wf->top == wf->n) wf->top = 0;
	f = &wf->a[wf->top];
	f->lo = lo, f->hi = hi;
	n = hi - lo + 1;
	kfree(km, f->mem);
	f->mem = Kmalloc(km, int32_t, 5 * (n + m2));
	f->H = f->mem + m1;
	f->E1 = f->H  + n + m2;
	f->F1 = f->E1 + n + m2;
	f->E2 = f->F1 + n + m2;
	f->F2 = f->E2 + n + m2;
	for (i = -m1; i < 0; ++i)
		f->H[i] = f->E1[i] = f->E2[i] = f->F1[i] = f->F2[i] = WF_NEG_INF;
	for (i = n; i < n + m1; ++i)
		f->H[i] = f->E1[i] = f->E2[i] = f->F1[i] = f->F2[i] = WF_NEG_INF;
	f->H -= lo, f->E1 -= lo, f->E2 -= lo, f->F1 -= lo, f->F2 -= lo; // such that f->H[lo] points to 0
}

static wf_stripe_t *wf_stripe_init(void *km, int32_t max_pen)
{
	int32_t i;
	wf_stripe_t *wf;
	wf = Kcalloc(km, wf_stripe_t, 1);
	wf->max_pen = max_pen;
	wf->n = max_pen + 1;
	wf->a = Kcalloc(km, wf_slice_t, wf->n);
	wf->lo = wf->hi = 0;
	for (i = 0; i < wf->n; ++i) {
		wf_slice_t *f;
		wf_stripe_add(km, wf, 0, 0);
		f = &wf->a[wf->top];
		f->H[0] = f->E1[0] = f->E2[0] = f->F1[0] = f->F2[0] = WF_NEG_INF;
	}
	wf->s = 0;
	wf->a[wf->top].H[0] = -1;
	return wf;
}

static void wf_stripe_destroy(void *km, wf_stripe_t *wf)
{
	int32_t i;
	for (i = 0; i < wf->n; ++i)
		kfree(km, wf->a[i].mem);
	kfree(km, wf);
}

static inline wf_slice_t *wf_stripe_get(wf_stripe_t *wf, int32_t x)
{
	int32_t y = wf->top - x;
	if (y < 0) y += wf->n;
	return &wf->a[y];
}

typedef struct {
	int32_t max_s, d;
} wf_chkpt_t;

/*
 * Extend a diagonal along exact matches
 */

// pad strings with distinct characters
static void wf_pad_str(void *km, int32_t tl, const char *ts, int32_t ql, const char *qs, char **pts, char **pqs)
{
	uint8_t t[256];
	int32_t i, c1 = -1, c2 = -1;
	char *s1, *s2;
	*pts = *pqs = 0;
	// collect all used characters
	memset(t, 0, 256);
	for (i = 0; i < tl; ++i)
		if (t[(uint8_t)ts[i]] == 0)
			t[(uint8_t)ts[i]] = 1;
	for (i = 0; i < ql; ++i)
		if (t[(uint8_t)qs[i]] == 0)
			t[(uint8_t)qs[i]] = 1;
	for (i = 0; i < 256; ++i)
		if (t[i] == 0) {
			if (c1 < 0) c1 = i;
			else if (c2 < 0) c2 = i;
		}
	if (c1 < 0 || c2 < 0) return; // The two strings use >=255 characters. Unlikely for bio strings.
	s1 = Kmalloc(km, char, tl + ql + 16); // the two strings are allocated together
	s2 = s1 + tl + 8;
	memcpy(s1, ts, tl);
	for (i = tl; i < tl + 8; ++i) s1[i] = c1; // pad with c1
	memcpy(s2, qs, ql);
	for (i = ql; i < ql + 8; ++i) s2[i] = c2; // pad with c2
	*pts = s1, *pqs = s2;
}

// Extend a diagonal along exact matches.
static inline int32_t wf_extend1_padded(const char *ts, const char *qs, int32_t k, int32_t d)
{
	uint64_t cmp = 0;
	const char *ts_ = ts + 1;
	const char *qs_ = qs + d + 1;
	while (1) {
		uint64_t x = *(uint64_t*)(ts_ + k); // warning: unaligned memory access
		uint64_t y = *(uint64_t*)(qs_ + k);
		cmp = x ^ y;
		if (cmp == 0) k += 8;
		else break;
	}
	k += __builtin_ctzl(cmp) >> 3;
	return k;
}

/*
 * Core wf_next() routines
 */

// Force loop vectorization. Learned from WFA.
#if defined(__clang__)
  #define PRAGMA_LOOP_VECTORIZE _Pragma("clang loop vectorize(enable)")
#elif defined(__GNUC__)
  #define PRAGMA_LOOP_VECTORIZE _Pragma("GCC ivdep")
#else
  #define PRAGMA_LOOP_VECTORIZE _Pragma("ivdep")
#endif

#define wf_max(a, b) ((a) >= (b)? (a) : (b))

static void wf_next_prep(void *km, const mwf_opt_t *opt, wf_stripe_t *wf, int32_t lo, int32_t hi,
						 int32_t **H, int32_t **E1, int32_t **F1, int32_t **E2, int32_t **F2,
						 const int32_t **pHx, const int32_t **pHo1, const int32_t **pHo2,
						 const int32_t **pE1, const int32_t **pF1, const int32_t **pE2, const int32_t **pF2)
{
	const wf_slice_t *fx, *fo1, *fo2, *fe1, *fe2;
	wf_slice_t *ft;
	wf_stripe_add(km, wf, lo, hi);
	ft  = &wf->a[wf->top];
	fx  = wf_stripe_get(wf, opt->x);
	fo1 = wf_stripe_get(wf, opt->o1 + opt->e1);
	fo2 = wf_stripe_get(wf, opt->o2 + opt->e2);
	fe1 = wf_stripe_get(wf, opt->e1);
	fe2 = wf_stripe_get(wf, opt->e2);
	*pHx = fx->H, *pHo1 = fo1->H, *pHo2 = fo2->H, *pE1 = fe1->E1, *pE2 = fe2->E2, *pF1 = fe1->F1, *pF2 = fe2->F2;
	*H = ft->H, *E1 = ft->E1, *E2 = ft->E2, *F1 = ft->F1, *F2 = ft->F2;
}

static void wf_next_score(int32_t lo, int32_t hi, int32_t *H, int32_t *E1, int32_t *F1, int32_t *E2, int32_t *F2,
						  const int32_t *pHx, const int32_t *pHo1, const int32_t *pHo2,
						  const int32_t *pE1, const int32_t *pF1, const int32_t *pE2, const int32_t *pF2)
{
	int32_t d;
	PRAGMA_LOOP_VECTORIZE
	for (d = lo; d <= hi; ++d) {
		int32_t h, f, e;
		E1[d] = wf_max(pHo1[d-1], pE1[d-1]);
		E2[d] = wf_max(pHo2[d-1], pE2[d-1]);
		e = wf_max(E1[d], E2[d]);
		F1[d] = wf_max(pHo1[d+1], pF1[d+1]) + 1;
		F2[d] = wf_max(pHo2[d+1], pF2[d+1]) + 1;
		f = wf_max(F1[d], F2[d]);
		h = wf_max(e, f);
		H[d] = wf_max(pHx[d] + 1, h);
		// if (H[d] >= -1) fprintf(stderr, "s=%d, d=%d, k=%d, (%d,%d)\n", wf->s, d, H[d], E1[d], F1[d]);
	}
}
static void wf_next_tb(int32_t lo, int32_t hi, int32_t *H, int32_t *E1, int32_t *F1, int32_t *E2, int32_t *F2, uint8_t *ax,
					   const int32_t *pHx, const int32_t *pHo1, const int32_t *pHo2,
					   const int32_t *pE1, const int32_t *pF1, const int32_t *pE2, const int32_t *pF2)
{
	int32_t d;
	PRAGMA_LOOP_VECTORIZE
	for (d = lo; d <= hi; ++d) {
		int32_t h, f, e;
		uint8_t x = 0, ze, zf, z;
		x |= pHo1[d-1] >= pE1[d-1]? 0 : 0x08;
		E1[d] = wf_max(pHo1[d-1], pE1[d-1]);
		x |= pHo2[d-1] >= pE2[d-1]? 0 : 0x20;
		E2[d] = wf_max(pHo2[d-1], pE2[d-1]);
		ze = E1[d] >= E2[d]? 1 : 3;
		e = wf_max(E1[d], E2[d]);
		x |= pHo1[d+1] >= pF1[d+1]? 0 : 0x10;
		F1[d] = wf_max(pHo1[d+1], pF1[d+1]) + 1;
		x |= pHo2[d+1] >= pF2[d+1]? 0 : 0x40;
		F2[d] = wf_max(pHo2[d+1], pF2[d+1]) + 1;
		zf = F1[d] >= F2[d]? 2 : 4;
		f = wf_max(F1[d], F2[d]);
		z = e >= f? ze : zf;
		h = wf_max(e, f);
		z = pHx[d] + 1 >= h? 0 : z;
		H[d] = wf_max(pHx[d] + 1, h);
		ax[d] = x | z;
	}
}

/*
 * Core algorithm
 */
static void wf_next_basic(void *km, void *km_tb, const mwf_opt_t *opt, wf_stripe_t *wf, wf_tb_t *tb, int32_t lo, int32_t hi)
{
	int32_t *H, *E1, *E2, *F1, *F2;
	const int32_t *pHx, *pHo1, *pHo2, *pE1, *pE2, *pF1, *pF2;
	wf_next_prep(km, opt, wf, lo, hi, &H, &E1, &F1, &E2, &F2, &pHx, &pHo1, &pHo2, &pE1, &pF1, &pE2, &pF2);
	if (tb) {
		uint8_t *ax;
		ax = wf_tb_add(km_tb, tb, lo, hi)->x - lo;
		wf_next_tb(lo, hi, H, E1, F1, E2, F2, ax, pHx, pHo1, pHo2, pE1, pF1, pE2, pF2);
	} else {
		wf_next_score(lo, hi, H, E1, F1, E2, F2, pHx, pHo1, pHo2, pE1, pF1, pE2, pF2);
	}
	if (H[lo] >= -1 || E1[lo] >= -1 || F1[lo] >= -1 || E2[lo] >= -1 || F2[lo] >= -1) wf->lo = lo;
	if (H[hi] >= -1 || E1[hi] >= -1 || F1[hi] >= -1 || E2[hi] >= -1 || F2[hi] >= -1) wf->hi = hi;
}

static uint32_t *wf_traceback(void *km, const mwf_opt_t *opt, wf_tb_t *tb, int32_t t_end, const char *ts, int32_t q_end, const char *qs, int32_t last, int32_t *n_cigar)
{
	wf_cigar_t cigar = {0,0,0};
	int32_t i = q_end, k = t_end, s = tb->n - 1;
	while (i >= 0 && k >= 0) {
		int32_t k0 = k, j, x, state, ext;
		if (last == 0) { // if the previous state is 0, check exact matches
			while (i >= 0 && k >= 0 && qs[i] == ts[k])
				--i, --k;
			if (k0 - k > 0)
				wf_cigar_push1(km, &cigar, 7, k0 - k);
			if (i < 0 || k < 0) break;
		}
		assert(s >= 0);
		j = i - k - tb->a[s].lo;
		assert(j <= tb->a[s].hi - tb->a[s].lo);
		x = tb->a[s].x[j];
		state = last == 0? x&7 : last;
		ext = state > 0? x>>(state+2)&1 : 0; // whether an extension
		//fprintf(stderr, "s=%d, %d->%d, ext=%d%d%d%d, i=%d, k=%d\n", s, last, state, x>>3&1, x>>4&1, x>>5&1, x>>6&1, i, k);
		if (state == 0) {
			wf_cigar_push1(km, &cigar, 8, 1);
			--i, --k, s -= opt->x;
		} else if (state == 1) {
			wf_cigar_push1(km, &cigar, 1, 1);
			--i, s -= ext? opt->e1 : opt->o1 + opt->e1;
		} else if (state == 3) {
			wf_cigar_push1(km, &cigar, 1, 1);
			--i, s -= ext? opt->e2 : opt->o2 + opt->e2;
		} else if (state == 2) {
			wf_cigar_push1(km, &cigar, 2, 1);
			--k, s -= ext? opt->e1 : opt->o1 + opt->e1;
		} else if (state == 4) {
			wf_cigar_push1(km, &cigar, 2, 1);
			--k, s -= ext? opt->e2 : opt->o2 + opt->e2;
		} else abort();
		last = state > 0 && ext? state : 0;
	}
	if (opt->flag&MWF_F_DEBUG) fprintf(stderr, "s0=%d, s=%d, i=%d, k=%d\n", tb->n-1, s, i, k);
	if (i >= 0) wf_cigar_push1(km, &cigar, 1, i + 1);
	else if (k >= 0) wf_cigar_push1(km, &cigar, 2, k + 1);
	for (i = 0; i < cigar.n>>1; ++i) { // reverse to the input order
		uint32_t t = cigar.cigar[i];
		cigar.cigar[i] = cigar.cigar[cigar.n - i - 1];
		cigar.cigar[cigar.n - i - 1] = t;
	}
	*n_cigar = cigar.n;
	return cigar.cigar;
}

// pts and pqs MUST BE padded with wf_pad_str()
static void mwf_wfa_core(void *km, const mwf_opt_t *opt, int32_t tl, const char *pts, int32_t ql, const char *pqs, int32_t n_seg, wf_chkpt_t *seg, mwf_rst_t *r)
{
	int32_t max_pen, sid, is_tb = !!(opt->flag&MWF_F_CIGAR), last_state = 0;
	wf_stripe_t *wf;
	wf_tb_t tb = {0,0,0};
	void *km_tb;

	memset(r, 0, sizeof(*r));
	km_tb = is_tb? km_init2(km, 8000000) : 0; // this is slightly smaller than the kalloc block size
	max_pen = opt->x;
	max_pen = max_pen > opt->o1 + opt->e1? max_pen : opt->o1 + opt->e1;
	max_pen = max_pen > opt->o2 + opt->e2? max_pen : opt->o2 + opt->e2;
	wf = wf_stripe_init(km, max_pen);
	assert(pts);

	sid = 0;
	while (1) {
		wf_slice_t *p = &wf->a[wf->top];
		int32_t d, lo, hi, *H = p->H;
		for (d = p->lo; d <= p->hi; ++d) {
			int32_t k = 0;
			if (H[d] < -1 || d + H[d] < -1 || H[d] >= tl || d + H[d] >= ql) continue;
			k = wf_extend1_padded(pts, pqs, H[d], d);
			//fprintf(stderr, "[s=%d] [%d,%d]:%d %d->%d,%d,%d,%d,%d\n", wf->s, p->lo, p->hi, d, H[d], k, wf->a[wf->top].E1[d], wf->a[wf->top].F1[d], wf->a[wf->top].E2[d], wf->a[wf->top].F2[d]);
			if (k == tl - 1 && d + k == ql - 1) {
				if (k == H[d] && is_tb)
					last_state = tb.a[tb.n-1].x[d - tb.a[tb.n-1].lo] & 7;
				break;
			}
			H[d] = k;
		}
		if (d <= p->hi) break;
		if (is_tb && seg && sid < n_seg && seg[sid].max_s == wf->s + 1) {
			//fprintf(stderr, "s=%d, [%d,%d], d=%d\n", wf->s + 1, lo, hi, seg[sid].d);
			assert(seg[sid].d >= p->lo && seg[sid].d <= p->hi);
			//lo = seg[sid].d - 1, hi = seg[sid].d + 1;
			++sid;
		}
		lo = wf->lo > -tl? wf->lo - 1 : -tl;
		hi = wf->hi <  ql? wf->hi + 1 :  ql;
		wf_next_basic(km, km_tb, opt, wf, is_tb? &tb : 0, lo, hi);
	}
	r->s = wf->s;
	if (km && (opt->flag&MWF_F_DEBUG)) {
		km_stat_t st;
		km_stat(km, &st);
		fprintf(stderr, "tl=%d, ql=%d, cap=%ld, avail=%ld, n_blks=%ld\n", tl, ql, st.capacity, st.available, st.n_blocks);
	}
	if (is_tb) {
		r->cigar = wf_traceback(km, opt, &tb, tl-1, pts, ql-1, pqs, last_state, &r->n_cigar);
		km_destroy(km_tb);
	}
	wf_stripe_destroy(km, wf);
}

/*
 * Low-memory mode
 */
typedef struct {
	int32_t n, n_intv, max_s;
	int32_t *x;
	uint64_t *intv;
} wf_ss_t; // snapshot

typedef struct {
	int32_t n, m;
	wf_ss_t *a;
} wf_sss_t;

static void wf_snapshot1(void *km, wf_stripe_t *sf, wf_ss_t *ss)
{
	int32_t j, k, t;
	ss->n = 0, ss->max_s = sf->s;
	for (j = 0; j < sf->n; ++j) {
		k = (sf->top + 1 + j) % sf->n;
		ss->n += 5 * (sf->a[k].hi - sf->a[k].lo + 1);
	}
	ss->x = Kmalloc(km, int32_t, ss->n);
	ss->n_intv = sf->n;
	ss->intv = Kmalloc(km, uint64_t, ss->n_intv);
	for (j = 0, t = 0; j < sf->n; ++j) {
		wf_slice_t *p;
		k = (sf->top + 1 + j) % sf->n;
		p = &sf->a[k];
		ss->intv[k] = (uint64_t)p->lo << 32 | (p->hi - p->lo + 1) * 5;
		for (k = p->lo; k <= p->hi; ++k) {
			ss->x[t] = p->H[k],  p->H[k]  = t++;
			ss->x[t] = p->E1[k], p->E1[k] = t++;
			ss->x[t] = p->F1[k], p->F1[k] = t++;
			ss->x[t] = p->E2[k], p->E2[k] = t++;
			ss->x[t] = p->F2[k], p->F2[k] = t++;
		}
	}
	assert(t == ss->n);
}

static void wf_snapshot(void *km, wf_sss_t *sss, wf_stripe_t *sf)
{
	if (sss->n == sss->m) {
		sss->m += (sss->m>>1) + 8;
		sss->a = Krealloc(km, wf_ss_t, sss->a, sss->m);
	}
	wf_snapshot1(km, sf, &sss->a[sss->n++]);
}

static void wf_snapshot_free(void *km, wf_sss_t *sss)
{
	int32_t j;
	for (j = 0; j < sss->n; ++j) {
		kfree(km, sss->a[j].x);
		kfree(km, sss->a[j].intv);
	}
	kfree(km, sss->a);
}

static void wf_next_seg(void *km, const mwf_opt_t *opt, uint8_t *xbuf, wf_stripe_t *wf, wf_stripe_t *sf, int32_t lo, int32_t hi)
{
	int32_t d, *H, *E1, *E2, *F1, *F2;
	const int32_t *pHx, *pHo1, *pHo2, *pE1, *pE2, *pF1, *pF2;
	uint8_t *ax = xbuf - lo;

	wf_next_prep(km, opt, wf, lo, hi, &H, &E1, &F1, &E2, &F2, &pHx, &pHo1, &pHo2, &pE1, &pF1, &pE2, &pF2);
	wf_next_tb(lo, hi, H, E1, F1, E2, F2, ax, pHx, pHo1, pHo2, pE1, pF1, pE2, pF2);
	wf_next_prep(km, opt, sf, lo, hi, &H, &E1, &F1, &E2, &F2, &pHx, &pHo1, &pHo2, &pE1, &pF1, &pE2, &pF2);
	//PRAGMA_LOOP_VECTORIZE
	for (d = lo; d <= hi; ++d) { // this loop can't be vectorized
		uint8_t x = ax[d];
		E1[d] = (x&0x08) == 0? pHo1[d-1] : pE1[d-1];
		F1[d] = (x&0x10) == 0? pHo1[d+1] : pF1[d+1];
		E2[d] = (x&0x20) == 0? pHo2[d-1] : pE2[d-1];
		F2[d] = (x&0x40) == 0? pHo2[d+1] : pF2[d+1];
		x &= 0x7;
		H[d] = x == 0? pHx[d] : x == 1? E1[d] : x == 2? F1[d] : x == 3? E2[d] : F2[d];
	}
	if (H[lo] >= -1 || E1[lo] >= -1 || F1[lo] >= -1 || E2[lo] >= -1 || F2[lo] >= -1) wf->lo = lo;
	if (H[hi] >= -1 || E1[hi] >= -1 || F1[hi] >= -1 || E2[hi] >= -1 || F2[hi] >= -1) wf->hi = hi;
}

static wf_chkpt_t *wf_traceback_seg(void *km, wf_sss_t *sss, int32_t last, int32_t *n_seg)
{
	int32_t j;
	wf_chkpt_t *seg;
	*n_seg = sss->n;
	seg = Kmalloc(km, wf_chkpt_t, sss->n);
	for (j = sss->n - 1; j >= 0; --j) {
		int32_t k, m;
		wf_ss_t *p = &sss->a[j];
		for (k = 0, m = 0; k < p->n_intv; ++k) {
			if (last >= m && last < m + (int32_t)p->intv[k])
				break;
			m += (int32_t)p->intv[k];
		}
		assert(k < p->n_intv);
		seg[j].max_s = p->max_s;
		seg[j].d = (int32_t)(p->intv[k]>>32) + (last - m) / 5;
		fprintf(stderr, "last=%d, d0=%d, k=%d, intv=%d, max_s=%d\n", last, (int32_t)(p->intv[k]>>32), k, p->n_intv, p->max_s);
		last = p->x[last];
	}
	fprintf(stderr, "last=%d\n", last);
	return seg;
}

wf_chkpt_t *mwf_wfa_seg(void *km, const mwf_opt_t *opt, int32_t tl, const char *pts, int32_t ql, const char *pqs, int32_t *n_seg_)
{
	int32_t lo = 0, hi = 0, max_pen, last, n_seg;
	wf_stripe_t *wf, *sf;
	wf_sss_t sss = {0,0,0};
	uint8_t *xbuf;
	wf_chkpt_t *seg, *tmp;

	max_pen = opt->x;
	max_pen = max_pen > opt->o1 + opt->e1? max_pen : opt->o1 + opt->e1;
	max_pen = max_pen > opt->o2 + opt->e2? max_pen : opt->o2 + opt->e2;
	xbuf = Kcalloc(km, uint8_t, tl + ql + 1);
	wf = wf_stripe_init(km, max_pen);
	sf = wf_stripe_init(km, max_pen);
	assert(pts);

	while (1) {
		int32_t d, *H = wf->a[wf->top].H;
		for (d = lo; d <= hi; ++d) {
			int32_t k;
			if (H[d] < -1 || d + H[d] < -1 || H[d] >= tl || d + H[d] >= ql) continue;
			k = wf_extend1_padded(pts, pqs, H[d], d);
			if (k == tl - 1 && d + k == ql - 1) {
				last = sf->a[sf->top].H[d];
				break;
			}
			H[d] = k;
		}
		if (d <= hi) break;
		if (lo > -tl) --lo;
		if (hi < ql) ++hi;
		if ((wf->s + 1) % opt->step == 0)
			wf_snapshot(km, &sss, sf);
		wf_next_seg(km, opt, xbuf, wf, sf, lo, hi);
	}
	seg = wf_traceback_seg(km, &sss, last, &n_seg);
	if (km && (opt->flag&MWF_F_DEBUG)) {
		km_stat_t st;
		km_stat(km, &st);
		fprintf(stderr, "tl=%d, ql=%d, cap=%ld, avail=%ld, n_blks=%ld\n", tl, ql, st.capacity, st.available, st.n_blocks);
	}
	wf_snapshot_free(km, &sss);
	wf_stripe_destroy(km, wf);
	wf_stripe_destroy(km, sf);
	kfree(km, xbuf);

	tmp = seg;
	seg = Kmalloc(km, wf_chkpt_t, n_seg); // this is to reduce memory fragmentation
	memcpy(seg, tmp, n_seg * sizeof(*seg));
	kfree(km, tmp);
	*n_seg_ = n_seg;
	return seg;
}

void mwf_wfa(void *km, const mwf_opt_t *opt, int32_t tl, const char *ts, int32_t ql, const char *qs, mwf_rst_t *r)
{
	int32_t n_seg = 0;
	wf_chkpt_t *seg = 0;
	char *pts, *pqs;

	wf_pad_str(km, tl, ts, ql, qs, &pts, &pqs);
	if (opt->step > 0)
		seg = mwf_wfa_seg(km, opt, tl, pts, ql, pqs, &n_seg);
	mwf_wfa_core(km, opt, tl, pts, ql, pqs, n_seg, seg, r);
	kfree(km, pts);
}
