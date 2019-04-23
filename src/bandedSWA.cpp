/*************************************************************************************
                    GNU GENERAL PUBLIC LICENSE
           		      Version 3, 29 June 2007

BWA-MEM2  (Sequence alignment using Burrows-Wheeler Transform),
Copyright (C) 2019  Vasimuddin Md, Sanchit Misra, Intel Corporation, Heng Li.
    
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License at https://www.gnu.org/licenses/ for more details.


TERMS AND CONDITIONS FOR DISTRIBUTION OF THE CODE
                                             
1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer. 
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution. 
3. Neither the name of Intel Corporation nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

Authors: Vasimuddin Md <vasimuddin.md@intel.com>; Sanchit Misra <sanchit.misra@intel.com>;
*****************************************************************************************/

#include "bandedSWA.h"
#ifdef VTUNE_ANALYSIS
#include <ittnotify.h> 
#endif

// ------------------------------------------------------------------------------------
// MACROs for vector code
extern uint64_t prof[10][112], data, SW_cells2;
#define AMBIG 4
#define DUMMY1 99
#define DUMMY2 100

//-----------------------------------------------------------------------------------
// constructor
BandedPairWiseSW::BandedPairWiseSW(const int o_del, const int e_del, const int o_ins,
								   const int e_ins, const int zdrop,
								   const int end_bonus, const int8_t *mat_,
								   int8_t w_match, int8_t w_mismatch, int numThreads)
{
	SW_cells = 0;
	mat = mat_;
	this->m = 5;
	//this->end_bonus = 5;
	this->end_bonus = end_bonus;
	this->zdrop = zdrop;
	this->w = 100;               // not needed,  remove during cleaning
	this->o_del = o_del;
	this->o_ins = o_ins;
	this->e_del = e_del;
	this->e_ins = e_ins;
	
	this->w_match	 = w_match;
	this->w_mismatch = w_mismatch*-1;
	this->w_open	 = o_del;  // redundant, used in vector code.
	this->w_extend	 = e_del;  // redundant, used in vector code.
	this->w_ambig	 = DEFAULT_AMBIG;
	this->swTicks = 0;
	setupTicks = 0;
	sort1Ticks = 0;
	swTicks = 0;
	sort2Ticks = 0;
	this->F8_ = this->H8_  = this->H8__ = NULL;
	this->F16_ = this->H16_  = this->H16__ = NULL;
	
	// int numThreads = 1;
	F8_ = H8_ = H8__ = NULL;
    F8_ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);
    H8_ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);
	H8__ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);

	F16_ = H16_ = H16__ = NULL;
    F16_ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);
    H16_ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);
	H16__ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);

	if (F8_ == NULL || H8_ == NULL || H8__ == NULL) {
		printf("BSW8 Memory not alloacted!!!\n"); exit(0);
	}   	
	if (F16_ == NULL || H16_ == NULL || H16__ == NULL) {
		printf("BSW16 Memory not alloacted!!!\n"); exit(0);
	}   	
}

// destructor 
BandedPairWiseSW::~BandedPairWiseSW() {
	_mm_free(F8_); _mm_free(H8_); _mm_free(H8__);
	_mm_free(F16_);_mm_free(H16_); _mm_free(H16__);
}

int64_t BandedPairWiseSW::getTicks()
{
    //printf("oneCount = %ld, totalCount = %ld\n", oneCount, totalCount);
    int64_t totalTicks = sort1Ticks + setupTicks + swTicks + sort2Ticks;
    printf("cost breakup: %ld, %ld, %ld, %ld, %ld\n",
            sort1Ticks, setupTicks, swTicks, sort2Ticks,
            totalTicks);

    return totalTicks;
}
// ------------------------------------------------------------------------------------
// Banded SWA - scalar code
// ------------------------------------------------------------------------------------

int BandedPairWiseSW::scalarBandedSWA(int qlen, const uint8_t *query,
									  int tlen, const uint8_t *target,
									  int w, int h0, int *_qle, int *_tle,
									  int *_gtle, int *_gscore,
									  int *_max_off) {
	
	// uint64_t sw_cells = 0;
	eh_t *eh; // score array
	int8_t *qp; // query profile
	int i, j, k, oe_del = o_del + e_del, oe_ins = o_ins + e_ins, beg, end, max, max_i, max_j, max_ins, max_del, max_ie, gscore, max_off;
	
	// assert(h0 > 0); //check !!!
	
	// allocate memory
	qp = (int8_t *) malloc(qlen * m);
	eh = (eh_t *) calloc(qlen + 1, 8);

	// generate the query profile
	for (k = i = 0; k < m; ++k) {
		const int8_t *p = &mat[k * m];
		//for (j = 0; j < qlen; ++j) qp[i++] = p[query[j]-48];  //sub 48
		for (j = 0; j < qlen; ++j) qp[i++] = p[query[j]];
	}

	// fill the first row
	eh[0].h = h0; eh[1].h = h0 > oe_ins? h0 - oe_ins : 0;
	for (j = 2; j <= qlen && eh[j-1].h > e_ins; ++j)
		eh[j].h = eh[j-1].h - e_ins;

	// adjust $w if it is too large
	k = m * m;
	for (i = 0, max = 0; i < k; ++i) // get the max score
		max = max > mat[i]? max : mat[i];
	max_ins = (int)((double)(qlen * max + end_bonus - o_ins) / e_ins + 1.);
	max_ins = max_ins > 1? max_ins : 1;
	w = w < max_ins? w : max_ins;
	max_del = (int)((double)(qlen * max + end_bonus - o_del) / e_del + 1.);
	max_del = max_del > 1? max_del : 1;
	w = w < max_del? w : max_del; // TODO: is this necessary?

	// DP loop
	max = h0, max_i = max_j = -1; max_ie = -1, gscore = -1;
	max_off = 0;
	beg = 0, end = qlen;
	for (i = 0; (i < tlen); ++i) {
		int t, f = 0, h1, m = 0, mj = -1;
		//int8_t *q = &qp[(target[i]-48) * qlen];   // sub 48
		int8_t *q = &qp[(target[i]) * qlen];
		// apply the band and the constraint (if provided)
		if (beg < i - w) beg = i - w;
		if (end > i + w + 1) end = i + w + 1;
		if (end > qlen) end = qlen;
		// compute the first column
		if (beg == 0) {
			h1 = h0 - (o_del + e_del * (i + 1));
			if (h1 < 0) h1 = 0;
		} else h1 = 0;
		for (j = beg; (j < end); ++j) {
			// At the beginning of the loop: eh[j] = { H(i-1,j-1), E(i,j) }, f = F(i,j) and h1 = H(i,j-1)
			// Similar to SSE2-SW, cells are computed in the following order:
			//   H(i,j)   = max{H(i-1,j-1)+S(i,j), E(i,j), F(i,j)}
			//   E(i+1,j) = max{H(i,j)-gapo, E(i,j)} - gape
			//   F(i,j+1) = max{H(i,j)-gapo, F(i,j)} - gape
			eh_t *p = &eh[j];
			int h, M = p->h, e = p->e; // get H(i-1,j-1) and E(i-1,j)
			p->h = h1;          // set H(i,j-1) for the next row
			M = M? M + q[j] : 0;// separating H and M to disallow a cigar like "100M3I3D20M"
			h = M > e? M : e;   // e and f are guaranteed to be non-negative, so h>=0 even if M<0
			h = h > f? h : f;
			h1 = h;             // save H(i,j) to h1 for the next column
			mj = m > h? mj : j; // record the position where max score is achieved
			m = m > h? m : h;   // m is stored at eh[mj+1]
			t = M - oe_del;
			t = t > 0? t : 0;
			e -= e_del;
			e = e > t? e : t;   // computed E(i+1,j)
			p->e = e;           // save E(i+1,j) for the next row
			t = M - oe_ins;
			t = t > 0? t : 0;
			f -= e_ins;
			f = f > t? f : t;   // computed F(i,j+1)
			SW_cells++;
		}
		eh[end].h = h1; eh[end].e = 0;
		if (j == qlen) {
			max_ie = gscore > h1? max_ie : i;
			gscore = gscore > h1? gscore : h1;
		}
		if (m == 0) break;
		if (m > max) {
			max = m, max_i = i, max_j = mj;
			max_off = max_off > abs(mj - i)? max_off : abs(mj - i);
		} else if (zdrop > 0) {
			if (i - max_i > mj - max_j) {
				if (max - m - ((i - max_i) - (mj - max_j)) * e_del > zdrop) break;
			} else {
				if (max - m - ((mj - max_j) - (i - max_i)) * e_ins > zdrop) break;
			}
		}
		// update beg and end for the next round
		for (j = beg; (j < end) && eh[j].h == 0 && eh[j].e == 0; ++j);
		beg = j;
		for (j = end; (j >= beg) && eh[j].h == 0 && eh[j].e == 0; --j);
		end = j + 2 < qlen? j + 2 : qlen;
		//beg = 0; end = qlen; // uncomment this line for debugging
	}
	free(eh); free(qp);
	if (_qle) *_qle = max_j + 1;
	if (_tle) *_tle = max_i + 1;
	if (_gtle) *_gtle = max_ie + 1;
	if (_gscore) *_gscore = gscore;
	if (_max_off) *_max_off = max_off;
	
#if MAXI
	fprintf(stderr, "%d (%d %d) %d %d %d\n", max, max_i+1, max_j+1, gscore, max_off, max_ie+1);
#endif

	// return sw_cells;
	return max;
}

// -------------------------------------------------------------
// Banded SWA, wrapper function
//-------------------------------------------------------------
void BandedPairWiseSW::scalarBandedSWAWrapper(SeqPair *seqPairArray,
											  uint8_t *seqBufRef,
											  uint8_t *seqBufQer,
											  int numPairs,
											  int nthreads,
											  int w) {

	for (int i=0; i<numPairs; i++) {
		//OutScore *o = outScoreArray + i;
		SeqPair *p = seqPairArray + i;

		//uint8_t *seq1 = seqBuf + p->id * 2 * MAX_SEQ_LEN;
        //uint8_t *seq2 = seqBuf + (p->id * 2 + 1) * MAX_SEQ_LEN;
		uint8_t *seq1 = seqBufRef + p->idr;
        uint8_t *seq2 = seqBufQer + p->idq;
		
		p->score = scalarBandedSWA(p->len2, seq2, p->len1,
								   seq1, w, p->h0, &p->qle, &p->tle,
								   &p->gtle, &p->gscore, &p->max_off);		
	}

}


#if ((!__AVX512BW__) & (__AVX2__))

//------------------------------------------------------------------------------
// MACROs
// ------------------------ vec-8 ---------------------------------------------
#define MAX_INSDEL8(qlen256, eb256, band256, band256_)					\
	{																	\
	__m256i max_ins256 = _mm256_add_epi8(qlen256, eb_ins256);			\
	__m256i cmp256 = _mm256_cmpgt_epi8(max_ins256, zero256);			\
	max_ins256 = _mm256_blendv_epi8(zero256, max_ins256, cmp256);		\
	cmp256 = _mm256_cmpgt_epi8(max_ins256, band256_);					\
	max_ins256 = _mm256_blendv_epi8(max_ins256, band256, cmp256);		\
	_mm256_store_si256((__m256i *) qlen, max_ins256);					\
	}

#define ZSCORE8(i4_256, y4_256)											\
	{																	\
		__m256i tmpi = _mm256_sub_epi8(i4_256, x256);					\
		__m256i tmpj = _mm256_sub_epi8(y4_256, y256);					\
		cmp = _mm256_cmpgt_epi8(tmpi, tmpj);							\
		score256 = _mm256_sub_epi8(maxScore256, maxRS1);				\
		__m256i insdel = _mm256_blendv_epi8(e_ins256, e_del256, cmp);	\
		__m256i sub_a256 = _mm256_sub_epi8(tmpi, tmpj);					\
		__m256i sub_b256 = _mm256_sub_epi8(tmpj, tmpi);					\
		tmp = _mm256_blendv_epi8(sub_b256, sub_a256, cmp);				\
		tmp = _mm256_sub_epi8(score256, tmp);							\
		cmp = _mm256_cmpgt_epi8(tmp, zdrop256);							\
		exit0 = _mm256_blendv_epi8(exit0, zero256, cmp);				\
	}

#define MAIN_CODE8_BAK(s1, s2, h00, h11, e11, f11, f21, zero256,  maxScore256, gapExtend256, y256, maxRS) \
	{																	\
		__m256i cmp11 = _mm256_cmpeq_epi8(s1, s2);						\
		__m256i sbt11 = _mm256_blendv_epi8(mismatch256, match256, cmp11); \
		__m256i tmp256 = _mm256_max_epu8(s1, s2);						\
		/*tmp256 = _mm256_cmpeq_epi8(tmp256, val102);*/					\
		sbt11 = _mm256_blendv_epi8(sbt11, w_ambig_256, tmp256);			\
		__m256i m11 = _mm256_add_epi8(h00, sbt11);						\
		cmp11 = _mm256_cmpeq_epi8(h00, zero256);						\
		m11 = _mm256_blendv_epi8(m11, zero256, cmp11);					\
		h11 = _mm256_max_epi8(m11, e11);								\
		h11 = _mm256_max_epi8(h11, f11);								\
		__m256i temp256 = _mm256_sub_epi8(m11, gapOE256);				\
		__m256i val256  = _mm256_max_epi8(temp256, zero256);			\
		e11 = _mm256_sub_epi8(e11, gapExtend256);						\
		e11 = _mm256_max_epi8(val256, e11);								\
		f21 = _mm256_sub_epi8(f11, gapExtend256);						\
		f21 = _mm256_max_epi8(val256, f21);								\
	}

#define MAIN_CODE8(s1, s2, h00, h11, e11, f11, f21, zero256,  maxScore256, e_ins256, oe_ins256, e_del256, oe_del256, gapExtend256, y256, maxRS) \
	{																	\
		__m256i cmp11 = _mm256_cmpeq_epi8(s1, s2);						\
		__m256i sbt11 = _mm256_blendv_epi8(mismatch256, match256, cmp11); \
		__m256i tmp256 = _mm256_max_epu8(s1, s2);						\
		/*tmp256 = _mm256_cmpeq_epi8(tmp256, val102);*/					\
		sbt11 = _mm256_blendv_epi8(sbt11, w_ambig_256, tmp256);			\
		__m256i m11 = _mm256_add_epi8(h00, sbt11);						\
		cmp11 = _mm256_cmpeq_epi8(h00, zero256);						\
		m11 = _mm256_blendv_epi8(m11, zero256, cmp11);					\
		h11 = _mm256_max_epi8(m11, e11);								\
		h11 = _mm256_max_epi8(h11, f11);								\
		__m256i temp256 = _mm256_sub_epi8(m11, oe_ins256);				\
		__m256i val256  = _mm256_max_epi8(temp256, zero256);			\
		e11 = _mm256_sub_epi8(e11, gapExtend256);						\
		e11 = _mm256_max_epi8(val256, e11);								\
		temp256 = _mm256_sub_epi8(m11, oe_del256);						\
		val256  = _mm256_max_epi8(temp256, zero256);					\
		f21 = _mm256_sub_epi8(f11, gapExtend256);						\
		f21 = _mm256_max_epi8(val256, f21);								\
	}

// ------------------------ vec 16 --------------------------------------------------
#define _mm256_blendv_epi16(a,b,c)				\
		_mm256_blendv_epi8(a, b, c);			


#define MAX_INSDEL16(qlen256, eb256, band256, band256_)					\
	{																	\
		__m256i max_ins256 = _mm256_add_epi16(qlen256, eb_ins256);		\
		__m256i cmp256 = _mm256_cmpgt_epi16(max_ins256, zero256);			\
		max_ins256 = _mm256_blendv_epi16(zero256, max_ins256, cmp256);	\
		cmp256 = _mm256_cmpgt_epi16(max_ins256, band256_);				\
		max_ins256 = _mm256_blendv_epi16(max_ins256, band256, cmp256);	\
		_mm256_store_si256((__m256i *) qlen, max_ins256);				\		
    }

#define ZSCORE16(i4_256, y4_256)											\
	{																	\
		__m256i tmpi = _mm256_sub_epi16(i4_256, x256);					\
		__m256i tmpj = _mm256_sub_epi16(y4_256, y256);					\
		cmp = _mm256_cmpgt_epi16(tmpi, tmpj);							\
		score256 = _mm256_sub_epi16(maxScore256, maxRS1);				\
		__m256i insdel = _mm256_blendv_epi16(e_ins256, e_del256, cmp);	\
		__m256i sub_a256 = _mm256_sub_epi16(tmpi, tmpj);					\
		__m256i sub_b256 = _mm256_sub_epi16(tmpj, tmpi);					\
		tmp = _mm256_blendv_epi16(sub_b256, sub_a256, cmp);				\
		tmp = _mm256_sub_epi16(score256, tmp);							\
		cmp = _mm256_cmpgt_epi16(tmp, zdrop256);							\
		exit0 = _mm256_blendv_epi16(exit0, zero256, cmp);				\
	}

#define MAIN_CODE16_BAK(s1, s2, h00, h11, e11, f11, f21, zero256,  maxScore256, gapExtend256, y256, maxRS) \
	{																	\
		__m256i cmp11 = _mm256_cmpeq_epi16(s1, s2);						\
		__m256i sbt11 = _mm256_blendv_epi16(mismatch256, match256, cmp11); \
		__m256i tmp256 = _mm256_max_epu16(s1, s2);						\
		sbt11 = _mm256_blendv_epi16(sbt11, w_ambig_256, tmp256);		\
		__m256i m11 = _mm256_add_epi16(h00, sbt11);						\
		cmp11 = _mm256_cmpeq_epi16(h00, zero256);						\
		m11 = _mm256_blendv_epi16(m11, zero256, cmp11);					\
		h11 = _mm256_max_epi16(m11, e11);								\
		h11 = _mm256_max_epi16(h11, f11);								\
		__m256i temp256 = _mm256_sub_epi16(m11, gapOE256);				\
		__m256i val256  = _mm256_max_epi16(temp256, zero256);			\
		e11 = _mm256_sub_epi16(e11, gapExtend256);						\
		e11 = _mm256_max_epi16(val256, e11);							\
		f21 = _mm256_sub_epi16(f11, gapExtend256);						\
		f21 = _mm256_max_epi16(val256, f21);							\
	}

#define MAIN_CODE16(s1, s2, h00, h11, e11, f11, f21, zero256,  maxScore256, e_ins256, oe_ins256, e_del256, oe_del256, gapExtend256, y256, maxRS) \
	{																	\
		__m256i cmp11 = _mm256_cmpeq_epi16(s1, s2);						\
		__m256i sbt11 = _mm256_blendv_epi16(mismatch256, match256, cmp11); \
		__m256i tmp256 = _mm256_max_epu16(s1, s2);						\
		sbt11 = _mm256_blendv_epi16(sbt11, w_ambig_256, tmp256);		\
		__m256i m11 = _mm256_add_epi16(h00, sbt11);						\
		cmp11 = _mm256_cmpeq_epi16(h00, zero256);						\
		m11 = _mm256_blendv_epi16(m11, zero256, cmp11);					\
		h11 = _mm256_max_epi16(m11, e11);								\
		h11 = _mm256_max_epi16(h11, f11);								\
		__m256i temp256 = _mm256_sub_epi16(m11, oe_ins256);				\
		__m256i val256  = _mm256_max_epi16(temp256, zero256);			\
		e11 = _mm256_sub_epi16(e11, gapExtend256);						\
		e11 = _mm256_max_epi16(val256, e11);							\
		temp256 = _mm256_sub_epi16(m11, oe_del256);						\
		val256  = _mm256_max_epi16(temp256, zero256);			\		
		f21 = _mm256_sub_epi16(f11, gapExtend256);						\
		f21 = _mm256_max_epi16(val256, f21);							\
	}

// MACROs section ends
// ------------------------------------------------------------------------------------



//----------------------------------------------------------------------------------
// B-SWA - Vector code
// ------------------------- AVX2 - 8 bit SIMD_LANES ---------------------------
inline void sortPairsLen(SeqPair *pairArray, int32_t count, SeqPair *tempArray,
						 int16_t *hist)
{

    int32_t i;
    __m256i zero256 = _mm256_setzero_si256();
    for(i = 0; i <= MAX_SEQ_LEN8; i+=16)
        _mm256_store_si256((__m256i *)(hist + i), zero256);
    
    for(i = 0; i < count; i++)
    {
        SeqPair sp = pairArray[i];
        hist[sp.len1]++;
		// histb[sp.len1]++;
		// hist[sp.len2]++;
		// hist[sp.h0]++;
    }

    int32_t cumulSum = 0;
    for(i = 0; i <= MAX_SEQ_LEN8; i++)
    {
        int32_t cur = hist[i];
        hist[i] = cumulSum;
		// histb[i] = cumulSum;
        cumulSum += cur;
    }

    for(i = 0; i < count; i++)
    {
        SeqPair sp = pairArray[i];
        int32_t pos = hist[sp.len1];
        tempArray[pos] = sp;
        hist[sp.len1]++;
    }

    for(i = 0; i < count; i++) {
        pairArray[i] = tempArray[i];
	}
}

inline void sortPairsId(SeqPair *pairArray, int32_t first, int32_t count,
						SeqPair *tempArray)
{

    int32_t i;

    for(i = 0; i < count; i++)
    {
        SeqPair sp = pairArray[i];
        int32_t pos = sp.id - first;
        tempArray[pos] = sp;
    }

    for(i = 0; i < count; i++)
        pairArray[i] = tempArray[i];	
}

/******************* Vector code, version 2.0 *************************/
#define PFD 2
void BandedPairWiseSW::getScores8(SeqPair *pairArray,
								  uint8_t *seqBufRef,
								  uint8_t *seqBufQer,
								  int32_t numPairs,
								  uint16_t numThreads,
								  int8_t w)
{
    int64_t startTick, endTick;
    // F8_ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);
    // H8_ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);
	// H8__ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);
	
	smithWatermanBatchWrapper8(pairArray, seqBufRef, seqBufQer, numPairs, numThreads, w);

	// _mm_free(F8_); _mm_free(H8_); _mm_free(H8__);

#if MAXI
	printf("AVX2 Vecor code: Writing output..\n");
	for (int l=0; l<numPairs; l++)
	{
		fprintf(stderr, "%d (%d %d) %d %d %d\n",
				pairArray[l].score, pairArray[l].tle, pairArray[l].qle,
				pairArray[l].gscore, pairArray[l].max_off, pairArray[l].gtle);
	}
	printf("Vector code: Writing output completed!!!\n\n");
#endif
	
}

/***
	# Notes: Version 2.0 of the function. I've added some functionality to version 1.0 here.
	# Use this function for practice.
 ***/

void BandedPairWiseSW::smithWatermanBatchWrapper8(SeqPair *pairArray,
												  uint8_t *seqBufRef,
												  uint8_t *seqBufQer,
												  int32_t numPairs,
												  uint16_t numThreads,
												  int8_t w)
{
	// printf("numThreads: %d %d\n", numThreads, omp_get_thread_num());

	int64_t st1, st2, st3, st4, st5;
    st1 = __rdtsc();
    uint8_t *seq1SoA = NULL;
	seq1SoA = (uint8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(uint8_t), 64);
	
    uint8_t *seq2SoA = NULL;
	seq2SoA = (uint8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(uint8_t), 64);
	
	if (seq1SoA == NULL || seq2SoA == NULL)
		printf("Mem not allocated!!!\n");
	

    int32_t ii;
    int32_t roundNumPairs = ((numPairs + SIMD_WIDTH8 - 1)/SIMD_WIDTH8 ) * SIMD_WIDTH8;
	assert(roundNumPairs < BATCH_SIZE * SEEDS_PER_READ);
    for(ii = numPairs; ii < roundNumPairs; ii++)
    {
        pairArray[ii].id = ii;
        pairArray[ii].len1 = 0;
        pairArray[ii].len2 = 0;
    }
		
    st2 = __rdtsc();	
#if SORT_PAIRS

    // Sort the sequences according to decreasing order of lengths
    SeqPair *tempArray = (SeqPair *)_mm_malloc(SORT_BLOCK_SIZE * numThreads *
											   sizeof(SeqPair), 64);
    int16_t *hist = (int16_t *)_mm_malloc((MAX_SEQ_LEN8 + 32) * numThreads *
										  sizeof(int16_t), 64);

#pragma omp parallel num_threads(numThreads)
    {
        int32_t tid = omp_get_thread_num();
        SeqPair *myTempArray = tempArray + tid * SORT_BLOCK_SIZE;
        int16_t *myHist = hist + tid * (MAX_SEQ_LEN8 + 32);

#pragma omp for
        for(ii = 0; ii < roundNumPairs; ii+=SORT_BLOCK_SIZE)
        {
            int32_t first, last;
            first = ii;
            last  = ii + SORT_BLOCK_SIZE;
            if(last > roundNumPairs) last = roundNumPairs;
            sortPairsLen(pairArray + first, last - first, myTempArray, myHist);
        }
    }
    _mm_free(hist);
#endif
	
    st3 = __rdtsc();

#ifdef VTUNE_ANALYSIS
    __itt_resume();
#endif

	int eb = end_bonus;
//#pragma omp parallel num_threads(numThreads)
    {
        int32_t i;
        // uint16_t tid = omp_get_thread_num();
		uint16_t tid = 0;
        uint8_t *mySeq1SoA = NULL;
		mySeq1SoA = seq1SoA + tid * MAX_SEQ_LEN8 * SIMD_WIDTH8;
        uint8_t *mySeq2SoA = NULL;
		mySeq2SoA = seq2SoA + tid * MAX_SEQ_LEN8 * SIMD_WIDTH8;
		assert(mySeq1SoA != NULL && mySeq2SoA != NULL);		
        uint8_t *seq1;
        uint8_t *seq2;
		uint8_t h0[SIMD_WIDTH8];
		uint8_t band[SIMD_WIDTH8];		
		uint8_t qlen[SIMD_WIDTH8] __attribute__((aligned(64)));
		int8_t bsize = 0;
		
		int8_t *H1 = H8_ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
		int8_t *H2 = H8__ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
		
		__m256i zero256	  = _mm256_setzero_si256();
		__m256i e_ins256  = _mm256_set1_epi8(e_ins);
		__m256i oe_ins256 = _mm256_set1_epi8(o_ins + e_ins);
		__m256i o_del256  = _mm256_set1_epi8(o_del);
		__m256i e_del256  = _mm256_set1_epi8(e_del);
		__m256i eb_ins256 = _mm256_set1_epi8(eb - o_ins);
		__m256i eb_del256 = _mm256_set1_epi8(eb - o_del);
		
		int8_t max = 0;
		if (max < w_match) max = w_match;
		if (max < w_mismatch) max = w_mismatch;
		if (max < w_ambig) max = w_ambig;
		
		int nstart = 0, nend = numPairs;

#if DEB
		int div = (spot-1)/32;
		nstart = div*32;
		nend = (div+1)*32;
		int lane = (spot - 1)%32;
		// nstart = 0;//, nend = spot;
		printf("nstart: %d, nend: %d\n", nstart, nend);
#endif
		
//#pragma omp for schedule(dynamic, 128)
		for(i = nstart; i < nend; i+=SIMD_WIDTH8)
		{
			// prof[4][0]++;
            int32_t j, k;
            uint8_t maxLen1 = 0;
            uint8_t maxLen2 = 0;
            //uint8_t minLen1 = MAX_SEQ_LEN8 + 1;
            //uint8_t minLen2 = MAX_SEQ_LEN8 + 1;
			//bsize = 100;
			bsize = w;
			
			uint64_t tim;
			// tim = _rdtsc();
            for(j = 0; j < SIMD_WIDTH8; j++)
            {
				{ // prefetch block
					SeqPair spf = pairArray[i + j + PFD];
					//_mm_prefetch((const char*) seqBufRef +  (int64_t)spf.id * MAX_SEQ_LEN_REF, 0);
					//_mm_prefetch((const char*) seqBufRef +  (int64_t)spf.id * MAX_SEQ_LEN_REF + 64, 0);
				}
                SeqPair sp = pairArray[i + j];
				h0[j] = sp.h0;
                //seq1 = seqBuf + 2 * (int64_t)sp.id * MAX_SEQ_LEN;
				// seq1 = seqBufRef + (int64_t)sp.id * MAX_SEQ_LEN_REF;
				seq1 = seqBufRef + (int64_t)sp.idr;

                for(k = 0; k < sp.len1; k++)
                {
                    mySeq1SoA[k * SIMD_WIDTH8 + j] = (seq1[k] == AMBIG?0xFF:seq1[k]);
					H2[k * SIMD_WIDTH8 + j] = 0;
                }
				qlen[j] = sp.len2 * max;
                if(maxLen1 < sp.len1) maxLen1 = sp.len1;
                // if(minLen1 > sp.len1) minLen1 = sp.len1;
            }
			// prof[1][tid] += _rdtsc() - tim;
			
			//maxLen1 = ((maxLen1 + 3) >> 2) * 4;
            for(j = 0; j < SIMD_WIDTH8; j++)
            {
                SeqPair sp = pairArray[i + j];
                for(k = sp.len1; k <= maxLen1; k++) //removed "="
                {
                    mySeq1SoA[k * SIMD_WIDTH8 + j] = DUMMY1;
					H2[k * SIMD_WIDTH8 + j] = DUMMY1;
                }
            }

//--------------------
			__m256i h0_256 = _mm256_load_si256((__m256i*) h0);
			_mm256_store_si256((__m256i *) H2, h0_256);
			__m256i tmp256 = _mm256_sub_epi8(h0_256, o_del256);

			for(k = 1; k < maxLen1; k++) {
				tmp256 = _mm256_sub_epi8(tmp256, e_del256);
				__m256i tmp256_ = _mm256_max_epi8(tmp256, zero256);
				_mm256_store_si256((__m256i *)(H2 + k* SIMD_WIDTH8), tmp256_);
			}
//-------------------
			// tim = _rdtsc();
            for(j = 0; j < SIMD_WIDTH8; j++)
            {
				{ // prefetch block
					SeqPair spf = pairArray[i + j + PFD];
					//_mm_prefetch((const char*) seqBufQer + (int64_t)spf.id * MAX_SEQ_LEN_QER, 0);
					//_mm_prefetch((const char*) seqBufQer + (int64_t)spf.id * MAX_SEQ_LEN_QER + 64, 0);
				}
				
                SeqPair sp = pairArray[i + j];

				// seq2 = seqBufQer + (int64_t)sp.id * MAX_SEQ_LEN_QER;
				seq2 = seqBufQer + (int64_t)sp.idq;
				
				if (sp.len2 > MAX_SEQ_LEN8) printf("Error: %d %d\n", sp.id, sp.len2);
				assert(sp.len2 < MAX_SEQ_LEN8);
				
                for(k = 0; k < sp.len2; k++)
                {
					mySeq2SoA[k * SIMD_WIDTH8 + j] = (seq2[k]==AMBIG?0xFF:seq2[k]);
					H1[k * SIMD_WIDTH8 + j] = 0;					
                }
                if(maxLen2 < sp.len2) maxLen2 = sp.len2;
            }
			// prof[1][tid] += _rdtsc() - tim;

            for(j = 0; j < SIMD_WIDTH8; j++)
            {
                SeqPair sp = pairArray[i + j];
                for(k = sp.len2; k <= maxLen2; k++)
                {
                    mySeq2SoA[k * SIMD_WIDTH8 + j] = DUMMY2;
					H1[k * SIMD_WIDTH8 + j] = 0;
                }
            }

//------------------------
			_mm256_store_si256((__m256i *) H1, h0_256);
			__m256i cmp256 = _mm256_cmpgt_epi8(h0_256, oe_ins256);
			tmp256 = _mm256_sub_epi8(h0_256, oe_ins256);
			// _mm256_store_si256((__m256i *) (H1 + SIMD_WIDTH8), tmp256);

			tmp256 = _mm256_blendv_epi8(zero256, tmp256, cmp256);
			_mm256_store_si256((__m256i *) (H1 + SIMD_WIDTH8), tmp256);
			for(k = 2; k < maxLen2; k++)
			{
				// __m256i h1_256 = _mm256_load_si256((__m256i *) (H1 + (k-1) * SIMD_WIDTH8));
				__m256i h1_256 = tmp256;
				tmp256 = _mm256_sub_epi8(h1_256, e_ins256);
				tmp256 = _mm256_max_epi8(tmp256, zero256);
				_mm256_store_si256((__m256i *)(H1 + k*SIMD_WIDTH8), tmp256);
				//printf("%d %d\n", k, H1[k*SIMD_WIDTH8 + lane]);
            }			
//------------------------
			/* Banding calculation in pre-processing */
#if 1
			uint8_t myband[SIMD_WIDTH8] __attribute__((aligned(64)));
			uint8_t temp[SIMD_WIDTH8] __attribute__((aligned(64)));
			{
				__m256i qlen256 = _mm256_load_si256((__m256i *) qlen);
				__m256i sum256 = _mm256_add_epi8(qlen256, eb_ins256);
				_mm256_store_si256((__m256i *) temp, sum256);				
				for (int l=0; l<SIMD_WIDTH8; l++) {
					double val = temp[l]/e_ins + 1.0;
					int max_ins = (int) val;
					max_ins = max_ins > 1? max_ins : 1;
					myband[l] = min(bsize, max_ins);
				}
				sum256 = _mm256_add_epi8(qlen256, eb_del256);
				_mm256_store_si256((__m256i *) temp, sum256);				
				for (int l=0; l<SIMD_WIDTH8; l++) {
					double val = temp[l]/e_del + 1.0;
					int max_ins = (int) val;
					max_ins = max_ins > 1? max_ins : 1;
					myband[l] = min(myband[l], max_ins);
					bsize = bsize < myband[l] ? myband[l] : bsize;
				}
			}
#endif
			
			// uint64_t tim_swa = _rdtsc();
            smithWaterman256_8(mySeq1SoA,
							   mySeq2SoA,
							   maxLen1,
							   maxLen2,
							   pairArray + i,
							   h0,
							   tid,
							   numPairs,
							   zdrop,
							   bsize,
							   qlen,
							   myband);
			// prof[0][tid] += _rdtsc() - tim_swa;
        }
    }

#ifdef VTUNE_ANALYSIS
    __itt_pause();
#endif
	
    st4 = __rdtsc();
	
#if SORT_PAIRS
	{
    // Sort the sequences according to increasing order of id
#pragma omp parallel num_threads(numThreads)
    {
        int32_t tid = omp_get_thread_num();
        SeqPair *myTempArray = tempArray + tid * SORT_BLOCK_SIZE;

#pragma omp for
        for(ii = 0; ii < roundNumPairs; ii+=SORT_BLOCK_SIZE)
        {
            int32_t first, last;
            first = ii;
            last  = ii + SORT_BLOCK_SIZE;
            if(last > roundNumPairs) last = roundNumPairs;
            sortPairsId(pairArray + first, first, last - first, myTempArray);
        }
    }
    _mm_free(tempArray);
	}
#endif
	
    st5 = __rdtsc();
    setupTicks = st2 - st1;
    sort1Ticks = st3 - st2;
    swTicks = st4 - st3;
    sort2Ticks = st5 - st4;

	// free mem
	_mm_free(seq1SoA);
	_mm_free(seq2SoA);
	
    return;
}


/***
	# Code perfectly working; all the 6 parameters and the DP matrix exactly matching the scalar output
	# Currently the performace is ~6X compare to original scalar code.
	## Output validated on SRR09 and 320M reads of SRR04.
 ***/
void BandedPairWiseSW::smithWaterman256_8(uint8_t seq1SoA[],
										  uint8_t seq2SoA[],
										  uint8_t nrow,
										  uint8_t ncol,
										  SeqPair *p,
										  uint8_t h0[],
										  uint16_t tid,
										  int32_t numPairs,
										  int zdrop,
										  uint8_t w,
										  uint8_t qlen[],
										  uint8_t myband[])
{
#if DEB
	int8_t Hmat[nrow + 10][ncol + 10], Fmat[nrow + 10][ncol + 10];;
#endif
	
	static int cntr = 0;
	cntr++;
	
	__m256i match256	 = _mm256_set1_epi8(this->w_match);
    __m256i mismatch256	 = _mm256_set1_epi8(this->w_mismatch);
    __m256i gapOpen256	 = _mm256_set1_epi8(this->w_open);
    __m256i gapExtend256 = _mm256_set1_epi8(this->w_extend);
	__m256i gapOE256	 = _mm256_set1_epi8(this->w_open + this->w_extend);
	__m256i w_ambig_256	 = _mm256_set1_epi8(this->w_ambig);	// ambig penalty
	__m256i five256		 = _mm256_set1_epi8(5);

	__m256i e_del256	= _mm256_set1_epi8(this->e_del);
	__m256i oe_del256	= _mm256_set1_epi8(this->o_del + this->e_del);
	__m256i e_ins256	= _mm256_set1_epi8(this->e_ins);
	__m256i oe_ins256	= _mm256_set1_epi8(this->o_ins + this->e_ins);
	
    int8_t	*F	= F8_ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
    int8_t	*H_h	= H8_ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
	int8_t	*H_v = H8__ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;

	int lane;
#if DEB
	lane = (spot - 1)%SIMD_WIDTH8;
#endif
	
    int8_t i, j;

	uint8_t tlen[SIMD_WIDTH8];
	uint8_t tail[SIMD_WIDTH8] __attribute((aligned(64)));
	uint8_t head[SIMD_WIDTH8] __attribute((aligned(64)));
	
	int minq = 1000;
	for (int l=0; l<SIMD_WIDTH8; l++) {
		tlen[l] = p[l].len1;
		qlen[l] = p[l].len2;
		if (p[l].len2 < minq) minq = p[l].len2;
	}
	minq -= 1; // for gscore

	__m256i tlen256 = _mm256_load_si256((__m256i *) tlen);
	__m256i qlen256 = _mm256_load_si256((__m256i *) qlen);
	__m256i myband256 = _mm256_load_si256((__m256i *) myband);
    __m256i zero256 = _mm256_setzero_si256();
	__m256i one256	= _mm256_set1_epi8(1);
	__m256i two256	= _mm256_set1_epi8(2);
	__m256i max_ie256 = zero256;
	__m256i ff256 = _mm256_set1_epi8(0xFF);
		
   	__m256i tail256 = qlen256, head256 = zero256;
	_mm256_store_si256((__m256i *) head, head256);
   	_mm256_store_si256((__m256i *) tail, tail256);
	//__m256i ib256 = _mm256_add_epi8(qlen256, qlen256);
	// ib256 = _mm256_sub_epi8(ib256, one256);

	__m256i mlen256 = _mm256_add_epi8(qlen256, myband256);
	mlen256 = _mm256_min_epu8(mlen256, tlen256);

	uint8_t temp[SIMD_WIDTH8]  __attribute((aligned(64)));
	uint8_t temp1[SIMD_WIDTH8]  __attribute((aligned(64)));
	
	__m256i s00	 = _mm256_load_si256((__m256i *)(seq1SoA));
	__m256i hval = _mm256_load_si256((__m256i *)(H_v));
	__mmask32 dmask = 0xFFFFFFFF;
	
	__m256i maxScore256 = hval;
    for(j = 0; j < ncol; j++)
		_mm256_store_si256((__m256i *)(F + j * SIMD_WIDTH8), zero256);
	
	__m256i x256 = zero256;
	__m256i y256 = zero256;
	__m256i i256 = zero256;
	__m256i gscore = _mm256_set1_epi8(-1);
	__m256i max_off256 = zero256;
	__m256i exit0 = _mm256_set1_epi8(0xFF);
	__m256i zdrop256 = _mm256_set1_epi8(zdrop);
	
	int beg = 0, end = ncol;
	int nbeg = beg, nend = end;
	
#if RDT
	uint64_t tim = _rdtsc();
#endif
	
    for(i = 0; i < nrow; i++)
    {		
        __m256i e11 = zero256;
        __m256i h00, h11, h10;
        __m256i s10 = _mm256_load_si256((__m256i *)(seq1SoA + (i + 0) * SIMD_WIDTH8));
		
		beg = nbeg; end = nend;
		int pbeg = beg;
		if (beg < i - w) beg = i - w;
		if (end > i + w + 1) end = i + w + 1;
		if (end > ncol) end = ncol;
		
		h10 = zero256;
		if (beg == 0)
			h10 = _mm256_load_si256((__m256i *)(H_v + (i+1) * SIMD_WIDTH8));
		
		__m256i j256 = zero256;
		__m256i maxRS1;
		maxRS1 = zero256;
		
		__m256i i1_256 = _mm256_set1_epi8(i+1);
		__m256i y1_256 = zero256;
		
#if RDT	
		uint64_t tim1 = _rdtsc();
#endif
		
		__m256i i256, cache256;
		__m256i phead256 = head256, ptail256 = tail256;
		i256 = _mm256_set1_epi8(i);
		cache256 = _mm256_sub_epi8(i256, myband256);
		head256 = _mm256_max_epi8(head256, cache256);
		cache256 = _mm256_add_epi8(i1_256, myband256);
		tail256 = _mm256_min_epu8(tail256, cache256);
		tail256 = _mm256_min_epu8(tail256, qlen256);
		
		// NEW, trimming.
		__m256i cmph = _mm256_cmpeq_epi8(head256, phead256);
		__m256i cmpt = _mm256_cmpeq_epi8(tail256, ptail256);
		// cmph &= cmpt;
		cmph = _mm256_and_si256(cmph, cmpt);
		__mmask32 cmp_ht = _mm256_movemask_epi8(cmph);
		
		for (int l=beg; l<end && cmp_ht != dmask; l++) {
			//for (int l=pbeg; l<beg; l++) {
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH8));
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH8));
			
			__m256i pj256 = _mm256_set1_epi8(l);
			__m256i j256 = _mm256_set1_epi8(l+1);
			__m256i cmp1 = _mm256_cmpgt_epi8(head256, pj256);
			uint32_t cval = _mm256_movemask_epi8(cmp1);
			if (cval == 0x00) break;
			//__m256i cmp2 = _mm256_cmpgt_epi8(pj256, tail256);
			__m256i cmp2 = _mm256_cmpgt_epi8(j256, tail256);
			cmp1 = _mm256_or_si256(cmp1, cmp2);
			h256 = _mm256_blendv_epi8(h256, zero256, cmp1);
			f256 = _mm256_blendv_epi8(f256, zero256, cmp1);
			
			_mm256_store_si256((__m256i *)(F + l * SIMD_WIDTH8), f256);
			_mm256_store_si256((__m256i *)(H_h + l * SIMD_WIDTH8), h256);
		}
		
#if RDT
		prof[DP3][0] += _rdtsc() - tim1;
#endif
		// beg = nbeg; end = nend;
		//__m256i cmp256_1 = _mm256_cmpgt_epi8(i1_256, tlen256);
		
		// beg = nbeg; end = nend;
		__m256i cmp256_1 = _mm256_cmpgt_epi8(i1_256, tlen256);
		
		__m256i cmpim = _mm256_cmpgt_epi8(i1_256, mlen256);
		__m256i cmpht = _mm256_cmpeq_epi8(tail256, head256);
		cmpim = _mm256_or_si256(cmpim, cmpht);
		// change
#if NEW
		cmpht = _mm256_cmpgt_epi8(head256, tail256);
		cmpim = _mm256_or_si256(cmpim, cmpht);
#endif
		// change end
		exit0 = _mm256_blendv_epi8(exit0, zero256, cmpim);
		
		
#if RDT
		tim1 = _rdtsc();
#endif
		
		j256 = _mm256_set1_epi8(beg);
#pragma unroll(4)
		for(j = beg; j < end; j++)
		{
            __m256i f11, f21, s2;
			h00 = _mm256_load_si256((__m256i *)(H_h + j * SIMD_WIDTH8));
            f11 = _mm256_load_si256((__m256i *)(F + j * SIMD_WIDTH8));
			
            s2 = _mm256_load_si256((__m256i *)(seq2SoA + (j) * SIMD_WIDTH8));
			
			__m256i pj256 = j256;
			j256 = _mm256_add_epi8(j256, one256);
			
            //MAIN_CODE8(s10, s2, h00, h11, e11, f11, f21, zero256,
			//		   maxScore256, gapExtend256, y1_256, maxRS1); //i+1
			MAIN_CODE8(s10, s2, h00, h11, e11, f11, f21, zero256,
					   maxScore256, e_ins256, oe_ins256,
					   e_del256, oe_del256, gapExtend256,
					   y1_256, maxRS1); //i+1

			
			// Masked writing
			__m256i cmp2 = _mm256_cmpgt_epi8(head256, pj256);
			__m256i cmp1 = _mm256_cmpgt_epi8(pj256, tail256);
			cmp1 = _mm256_or_si256(cmp1, cmp2);
			h10 = _mm256_blendv_epi8(h10, zero256, cmp1);
			f21 = _mm256_blendv_epi8(f21, zero256, cmp1);
			
			__m256i bmaxRS = maxRS1;										
			maxRS1 =_mm256_max_epi8(maxRS1, h11);							
			__m256i cmpA = _mm256_cmpgt_epi8(maxRS1, bmaxRS);					
			__m256i cmpB =_mm256_cmpeq_epi8(maxRS1, h11);					
			cmpA = _mm256_or_si256(cmpA, cmpB);
			cmp1 = _mm256_cmpgt_epi8(j256, tail256); // change
			cmp1 = _mm256_or_si256(cmp1, cmp2);		  // change	 
			cmpA = _mm256_blendv_epi8(y1_256, j256, cmpA);
			y1_256 = _mm256_blendv_epi8(cmpA, y1_256, cmp1);
			maxRS1 = _mm256_blendv_epi8(maxRS1, bmaxRS, cmp1);						
			
			_mm256_store_si256((__m256i *)(F + j * SIMD_WIDTH8), f21);
			_mm256_store_si256((__m256i *)(H_h + j * SIMD_WIDTH8), h10);
			
			h10 = h11;
			
			//j256 = _mm256_add_epi8(j256, one256);
			
			if (j >= minq)
			{
				__m256i cmp = _mm256_cmpeq_epi8(j256, qlen256);
				__m256i max_gh = _mm256_max_epi8(gscore, h11);
				__m256i cmp_gh = _mm256_cmpgt_epi8(gscore, h11);
				__m256i tmp256_1 = _mm256_blendv_epi8(i1_256, max_ie256, cmp_gh);
				
				tmp256_1 = _mm256_blendv_epi8(max_ie256, tmp256_1, cmp);
				tmp256_1 = _mm256_blendv_epi8(max_ie256, tmp256_1, exit0);
				
				max_gh = _mm256_blendv_epi8(gscore, max_gh, exit0);
				max_gh = _mm256_blendv_epi8(gscore, max_gh, cmp);				
				
				cmp = _mm256_cmpgt_epi8(j256, tail256); 
				max_gh = _mm256_blendv_epi8(max_gh, gscore, cmp);
				max_ie256 = _mm256_blendv_epi8(tmp256_1, max_ie256, cmp);
				gscore = max_gh;
			}
        }
		_mm256_store_si256((__m256i *)(H_h + j * SIMD_WIDTH8), h10);
		_mm256_store_si256((__m256i *)(F + j * SIMD_WIDTH8), zero256);
		
		
		/* exit due to zero score by a row */
		uint32_t cval = 0;
		__m256i bmaxScore256 = maxScore256;
		__m256i tmp = _mm256_cmpeq_epi8(maxRS1, zero256);
		cval = _mm256_movemask_epi8(tmp);
		if (cval == 0xFFFFFFFF) break;
		exit0 = _mm256_blendv_epi8(exit0, zero256,  tmp);
		
		__m256i score256 = _mm256_max_epi8(maxScore256, maxRS1);
		maxScore256 = _mm256_blendv_epi8(maxScore256, score256, exit0);
		
		__m256i cmp = _mm256_cmpgt_epi8(maxScore256, bmaxScore256);
		y256 = _mm256_blendv_epi8(y256, y1_256, cmp);
		x256 = _mm256_blendv_epi8(x256, i1_256, cmp);		
		// max_off calculations
		tmp = _mm256_sub_epi8(y1_256, i1_256);
		tmp = _mm256_abs_epi8(tmp);
		__m256i bmax_off256 = max_off256;
		tmp = _mm256_max_epi8(max_off256, tmp);
		max_off256 = _mm256_blendv_epi8(bmax_off256, tmp, cmp);
		
		// Z-score
		ZSCORE8(i1_256, y1_256);		
		
#if RDT
		prof[DP1][0] += _rdtsc() - tim1;
		tim1 = _rdtsc();
#endif


/* Narrowing of the band */
		/* From beg */
		int l;
 		for (l = beg; l < end; l++) {
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH8));
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH8));
			__m256i tmp = _mm256_or_si256(f256, h256);
			tmp = _mm256_cmpeq_epi8(tmp, zero256);
			uint32_t val = _mm256_movemask_epi8(tmp);
			if (val == 0xFFFFFFFF) nbeg = l;
			else
				break;
		}
		
		/* From end */
		bool flg = 1;
		for (l = end; l >= beg; l--) {
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH8));
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH8));
			__m256i tmp = _mm256_or_si256(f256, h256);
			tmp = _mm256_cmpeq_epi8(tmp, zero256);
			uint32_t val = _mm256_movemask_epi8(tmp);
			if (val != 0xFFFFFFFF && flg)  
				break;
		}
		// int pnend =nend;
		nend = l + 2 < ncol? l + 2: ncol;
		
		__m256i tail256_ = _mm256_sub_epi8(tail256, one256);
		__m256i tmpb = ff256;
		
		__m256i exit1 = _mm256_xor_si256(exit0, ff256);
		__m256i l256 = _mm256_set1_epi8(beg);
		for (l = beg; l < end; l++) {
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH8));
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH8));
			
			__m256i tmp = _mm256_or_si256(f256, h256);
			tmp = _mm256_or_si256(tmp, exit1);			
			tmp = _mm256_cmpeq_epi8(tmp, zero256);
			uint32_t val = _mm256_movemask_epi8(tmp);
			if (val == 0x00) {
				break;
			}
			tmp = _mm256_and_si256(tmp,tmpb);
			//__m256i l256 = _mm256_set1_epi8(l+1);
			l256 = _mm256_add_epi8(l256, one256);
			
#if !NEW
			__m256i mask2 = _mm256_cmpgt_epi8(l256, tail256_);
			mask2 = _mm256_blendv_epi8(l256, tail256,  mask2);
			head256 = _mm256_blendv_epi8(head256, mask2, tmp);
#else
			head256 = _mm256_blendv_epi8(head256, l256, tmp);
#endif
			tmpb = tmp;			
		}
		// _mm256_store_si256((__m256i *) head, head256);
		
		__m256i  index256 = tail256;
		tmpb = ff256;
		
		l256 = _mm256_set1_epi8(end);
		for (l = end; l >= beg; l--)
		{
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH8));
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH8));
			
			__m256i tmp = _mm256_or_si256(f256, h256);
			tmp = _mm256_or_si256(tmp, exit1);
			tmp = _mm256_cmpeq_epi8(tmp, zero256);			
			uint32_t val = _mm256_movemask_epi8(tmp);
			if (val == 0x00)  {
				break;
			}
			
			tmp = _mm256_and_si256(tmp,tmpb);
			l256 = _mm256_sub_epi8(l256, one256);
			
#if !NEW
			__m256i mask2 = _mm256_cmpgt_epi8(l256, tail256);		
			mask2 = _mm256_blendv_epi8(l256, tail256,  mask2);
			index256 = _mm256_blendv_epi8(index256, mask2, tmp);
#else
			index256 = _mm256_blendv_epi8(index256, l256, tmp);
#endif
			tmpb = tmp;
		}
		index256 = _mm256_add_epi8(index256, two256);
		tail256 = _mm256_min_epi8(index256, qlen256);
		// _mm256_store_si256((__m256i *) tail, tail256);		

#if RDT
		prof[DP2][0] += _rdtsc() - tim1;
#endif
    }
	
#if RDT
	prof[DP][0] += _rdtsc() - tim;
#endif
	
    int8_t score[SIMD_WIDTH8]  __attribute((aligned(64)));
	_mm256_store_si256((__m256i *) score, maxScore256);

	int8_t maxi[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) maxi, x256);

	int8_t maxj[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) maxj, y256);

	int8_t max_off_ar[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) max_off_ar, max_off256);

	int8_t gscore_ar[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) gscore_ar, gscore);

	int8_t maxie_ar[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) maxie_ar, max_ie256);
	
    for(i = 0; i < SIMD_WIDTH8; i++)
    {
		p[i].score = score[i];
		p[i].tle = maxi[i];
		p[i].qle = maxj[i];
		p[i].max_off = max_off_ar[i];
		p[i].gscore = gscore_ar[i];
		p[i].gtle = maxie_ar[i];
    }
	
    return;
}

// ------------------------- AVX2 - 16 bit SIMD_LANES ---------------------------
#define PFD 2
void BandedPairWiseSW::getScores16(SeqPair *pairArray,
								   uint8_t *seqBufRef,
								   uint8_t *seqBufQer,
								   int32_t numPairs,
								   uint16_t numThreads,
								   int8_t w)
{
    int64_t startTick, endTick;
	 // F16_ = H16_ = H16__ = NULL;
     // F16_ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);
     // H16_ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);
	 // H16__ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);
	 // if (F16_ == NULL || H16_ == NULL || H16__ == NULL) {
	 // 	printf("Memory not alloacted!!!\n");
	 // 	exit(0);
	 // }   

	smithWatermanBatchWrapper16(pairArray, seqBufRef, seqBufQer, numPairs, numThreads, w);

	// _mm_free(F16_);_mm_free(H16_); _mm_free(H16__);

#if MAXI
	printf("AVX2 Vecor code: Writing output..\n");
	for (int l=0; l<numPairs; l++)
	{
		fprintf(stderr, "%d (%d %d) %d %d %d\n",
				pairArray[l].score, pairArray[l].x, pairArray[l].y,
				pairArray[l].gscore, pairArray[l].max_off, pairArray[l].max_ie);

	}
	printf("Vector code: Writing output completed!!!\n\n");
#endif
	
}

/***
	# Notes: Version 2.0 of the function. I've added some functionality to version 1.0 here.
	# Use this function for practice.
 ***/
void BandedPairWiseSW::smithWatermanBatchWrapper16(SeqPair *pairArray,
												   uint8_t *seqBufRef,
												   uint8_t *seqBufQer,
												   int32_t numPairs,
												   uint16_t numThreads,
												   int8_t w)
{
	// printf("numThreads: %d\n", numThreads);	
	int64_t st1, st2, st3, st4, st5;
	
    st1 = __rdtsc();
    uint16_t *seq1SoA = (uint16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(uint16_t), 64);
    uint16_t *seq2SoA = (uint16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(uint16_t), 64);

    int32_t ii;
    int32_t roundNumPairs = ((numPairs + SIMD_WIDTH16 - 1)/SIMD_WIDTH16 ) * SIMD_WIDTH16;
    for(ii = numPairs; ii < roundNumPairs; ii++)
    {
        pairArray[ii].id = ii;
        pairArray[ii].len1 = 0;
        pairArray[ii].len2 = 0;
    }

    st2 = __rdtsc();	
#if SORT_PAIRS
    // Sort the sequences according to decreasing order of lengths
    SeqPair *tempArray = (SeqPair *)_mm_malloc(SORT_BLOCK_SIZE * numThreads *
											   sizeof(SeqPair), 64);
    int16_t *hist = (int16_t *)_mm_malloc((MAX_SEQ_LEN16 + 16) * numThreads *
										  sizeof(int16_t), 64);
	int16_t *histb = (int16_t *)_mm_malloc((MAX_SEQ_LEN16 + 16) * numThreads *
										   sizeof(int16_t), 64);
#pragma omp parallel num_threads(numThreads)
    {
        int32_t tid = omp_get_thread_num();
        SeqPair *myTempArray = tempArray + tid * SORT_BLOCK_SIZE;
        int16_t *myHist = hist + tid * (MAX_SEQ_LEN16 + 16);
		int16_t *myHistb = histb + tid * (MAX_SEQ_LEN16 + 16);

#pragma omp for
        for(ii = 0; ii < roundNumPairs; ii+=SORT_BLOCK_SIZE)
        {
            int32_t first, last;
            first = ii;
            last  = ii + SORT_BLOCK_SIZE;
            if(last > roundNumPairs) last = roundNumPairs;
            sortPairsLen(pairArray + first, last - first, myTempArray, myHist, myHistb);
        }
    }
    _mm_free(hist);
#endif
    st3 = __rdtsc();

#ifdef VTUNE_ANALYSIS
    __itt_resume();
#endif

	int eb = end_bonus;
#pragma omp parallel num_threads(numThreads)
    {
        int32_t i;
        uint16_t tid = omp_get_thread_num();
        uint16_t *mySeq1SoA = seq1SoA + tid * MAX_SEQ_LEN16 * SIMD_WIDTH16;
        uint16_t *mySeq2SoA = seq2SoA + tid * MAX_SEQ_LEN16 * SIMD_WIDTH16;
        uint8_t *seq1;
        uint8_t *seq2;
		uint16_t h0[SIMD_WIDTH16];
		uint16_t band[SIMD_WIDTH16];		
		uint16_t qlen[SIMD_WIDTH16] __attribute__((aligned(64)));
		int16_t bsize = 0;
		
		int16_t *H1 = H16_ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;
		int16_t *H2 = H16__ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;
		
		__m256i zero256	  = _mm256_setzero_si256();
		__m256i e_ins256  = _mm256_set1_epi16(e_ins);
		__m256i oe_ins256 = _mm256_set1_epi16(o_ins + e_ins);
		__m256i o_del256  = _mm256_set1_epi16(o_del);
		__m256i e_del256  = _mm256_set1_epi16(e_del);
		__m256i eb_ins256 = _mm256_set1_epi16(eb - o_ins);
		__m256i eb_del256 = _mm256_set1_epi16(eb - o_del);
		
		int16_t max = 0;
		if (max < w_match) max = w_match;
		if (max < w_mismatch) max = w_mismatch;
		if (max < w_ambig) max = w_ambig;
		
		int nstart = 0, nend = numPairs;

#if DEB
		int div = (spot-1)/SIMD_WIDTH16;
		nstart = div*SIMD_WIDTH16;
		nend = (div+1)*SIMD_WIDTH16;
		int lane = (spot - 1)%SIMD_WIDTH16;
		nstart = 0;//, nend = spot;
		printf("nstart: %d, nend: %d\n", nstart, nend);
#endif

#pragma omp for schedule(dynamic, 128)
		for(i = nstart; i < nend; i+=SIMD_WIDTH16)
		{
			// prof[4][0]++;
            int32_t j, k;
            uint16_t maxLen1 = 0;
            uint16_t maxLen2 = 0;
            //uint16_t minLen1 = MAX_SEQ_LEN16 + 1;
            //uint16_t minLen2 = MAX_SEQ_LEN16 + 1;
			//bsize = 100;
			bsize = w;

			uint64_t tim;
			// tim = _rdtsc();
            for(j = 0; j < SIMD_WIDTH16; j++)
            {
				{ // prefetch block
					SeqPair spf = pairArray[i + j + PFD];
					//_mm_prefetch((const char*) seqBufRef + (int64_t)spf.id * MAX_SEQ_LEN_REF, 0);
					//_mm_prefetch((const char*) seqBufRef + (int64_t)spf.id * MAX_SEQ_LEN_REF + 64, 0);
					_mm_prefetch((const char*) seqBufRef + (int64_t)spf.idr, 0);
					_mm_prefetch((const char*) seqBufRef + (int64_t)spf.idr + 64, 0);
				}

                SeqPair sp = pairArray[i + j];
				h0[j] = sp.h0;
                //seq1 = seqBufRef + (int64_t)sp.id * MAX_SEQ_LEN_REF;
				seq1 = seqBufRef + (int64_t)sp.idr;
				
                for(k = 0; k < sp.len1; k++)
                {
                    mySeq1SoA[k * SIMD_WIDTH16 + j] = (seq1[k] == AMBIG?0xFFFF:seq1[k]);
					H2[k * SIMD_WIDTH16 + j] = 0;
                }
				qlen[j] = sp.len2 * max;
                if(maxLen1 < sp.len1) maxLen1 = sp.len1;
            }
			// prof[1][tid] += _rdtsc() - tim;
		
            for(j = 0; j < SIMD_WIDTH16; j++)
            {
                SeqPair sp = pairArray[i + j];
                for(k = sp.len1; k <= maxLen1; k++) //removed "="
                {
                    mySeq1SoA[k * SIMD_WIDTH16 + j] = DUMMY1;
					H2[k * SIMD_WIDTH16 + j] = DUMMY1;
                }
            }
//--------------------
			__m256i h0_256 = _mm256_load_si256((__m256i*) h0);
			_mm256_store_si256((__m256i *) H2, h0_256);
			__m256i tmp256 = _mm256_sub_epi16(h0_256, o_del256);

			for(k = 1; k < maxLen1; k++) {
				tmp256 = _mm256_sub_epi16(tmp256, e_del256);
				__m256i tmp256_ = _mm256_max_epi16(tmp256, zero256);
				_mm256_store_si256((__m256i *)(H2 + k* SIMD_WIDTH16), tmp256_);
			}
//-------------------
			// tim = _rdtsc();
            for(j = 0; j < SIMD_WIDTH16; j++)
            {
				{ // prefetch block
					SeqPair spf = pairArray[i + j + PFD];
					//_mm_prefetch((const char*) seqBufQer + (int64_t)spf.id * MAX_SEQ_LEN_QER, 0);
					//_mm_prefetch((const char*) seqBufQer + (int64_t)spf.id * MAX_SEQ_LEN_QER + 64, 0);
					_mm_prefetch((const char*) seqBufQer + (int64_t)spf.idq, 0);
					_mm_prefetch((const char*) seqBufQer + (int64_t)spf.idq + 64, 0);
				}
				
                SeqPair sp = pairArray[i + j];
                //seq2 = seqBufQer + (int64_t)sp.id * MAX_SEQ_LEN_QER;
				seq2 = seqBufQer + (int64_t)sp.idq;				
                for(k = 0; k < sp.len2; k++)
                {
                    mySeq2SoA[k * SIMD_WIDTH16 + j] = (seq2[k]==AMBIG?0xFFFF:seq2[k]);
					H1[k * SIMD_WIDTH16 + j] = 0;					
                }
                if(maxLen2 < sp.len2) maxLen2 = sp.len2;
                // if(minLen2 > sp.len2) minLen2 = sp.len2;
            }
			// prof[1][tid] += _rdtsc() - tim;
			
            for(j = 0; j < SIMD_WIDTH16; j++)
            {
                SeqPair sp = pairArray[i + j];
                for(k = sp.len2; k <= maxLen2; k++)
                {
                    mySeq2SoA[k * SIMD_WIDTH16 + j] = DUMMY2;
					H1[k * SIMD_WIDTH16 + j] = 0;
                }
            }
//------------------------
			_mm256_store_si256((__m256i *) H1, h0_256);
			__m256i cmp256 = _mm256_cmpgt_epi16(h0_256, oe_ins256);
			tmp256 = _mm256_sub_epi16(h0_256, oe_ins256);

			tmp256 = _mm256_blendv_epi16(zero256, tmp256, cmp256);
			_mm256_store_si256((__m256i *) (H1 + SIMD_WIDTH16), tmp256);
			for(k = 2; k < maxLen2; k++)
			{
				__m256i h1_256 = tmp256;
				tmp256 = _mm256_sub_epi16(h1_256, e_ins256);
				tmp256 = _mm256_max_epi16(tmp256, zero256);
				_mm256_store_si256((__m256i *)(H1 + k*SIMD_WIDTH16), tmp256);
            }
//------------------------
#if 1
			uint16_t myband[SIMD_WIDTH16] __attribute__((aligned(64)));
			uint16_t temp[SIMD_WIDTH16] __attribute__((aligned(64)));
			{
				__m256i qlen256 = _mm256_load_si256((__m256i *) qlen);
				__m256i sum256 = _mm256_add_epi16(qlen256, eb_ins256);
				_mm256_store_si256((__m256i *) temp, sum256);				
				for (int l=0; l<SIMD_WIDTH16; l++) {
					double val = temp[l]/e_ins + 1.0;
					int max_ins = val;
					max_ins = max_ins > 1? max_ins : 1;
					myband[l] = min(bsize, max_ins);
				}
				sum256 = _mm256_add_epi16(qlen256, eb_del256);
				_mm256_store_si256((__m256i *) temp, sum256);				
				for (int l=0; l<SIMD_WIDTH16; l++) {
					double val = temp[l]/e_del + 1.0;
					int max_ins = val;
					max_ins = max_ins > 1? max_ins : 1;
					myband[l] = min(myband[l], max_ins);
					bsize = bsize < myband[l] ? myband[l] : bsize;					
				}
			}
#endif

			// uint64_t tim_swa = _rdtsc();
            smithWaterman256_16(mySeq1SoA,
								mySeq2SoA,
								maxLen1,
								maxLen2,
								pairArray + i,
								h0,
								tid,
								numPairs,
								zdrop,
								bsize, 
								qlen,
								myband);
			// prof[0][tid] += _rdtsc() - tim_swa;
        }
    }

#ifdef VTUNE_ANALYSIS
    __itt_pause();
#endif
	
    st4 = __rdtsc();
#if SORT_PAIRS
	{
    // Sort the sequences according to increasing order of id
#pragma omp parallel num_threads(numThreads)
    {
        int32_t tid = omp_get_thread_num();
        SeqPair *myTempArray = tempArray + tid * SORT_BLOCK_SIZE;

#pragma omp for
        for(ii = 0; ii < roundNumPairs; ii+=SORT_BLOCK_SIZE)
        {
            int32_t first, last;
            first = ii;
            last  = ii + SORT_BLOCK_SIZE;
            if(last > roundNumPairs) last = roundNumPairs;
            sortPairsId(pairArray + first, first, last - first, myTempArray);
        }
    }
    _mm_free(tempArray);
	}
#endif

	
    st5 = __rdtsc();
    setupTicks += st2 - st1;
    sort1Ticks += st3 - st2;
    swTicks += st4 - st3;
    sort2Ticks += st5 - st4;

	// free mem
	_mm_free(seq1SoA);
	_mm_free(seq2SoA);
	
    return;
}

void BandedPairWiseSW::smithWaterman256_16(uint16_t seq1SoA[],
										   uint16_t seq2SoA[],
										   uint16_t nrow,
										   uint16_t ncol,
										   SeqPair *p,
										   uint16_t h0[],
										   uint16_t tid,
										   int32_t numPairs,
										   int zdrop,
										   uint16_t w,
										   uint16_t qlen[],
										   uint16_t myband[])
{
#if DEB
	int16_t Hmat[nrow + 10][ncol + 10], Fmat[nrow + 10][ncol + 10];;
#endif

    __m256i match256	 = _mm256_set1_epi16(this->w_match);
    __m256i mismatch256	 = _mm256_set1_epi16(this->w_mismatch);
    __m256i gapOpen256	 = _mm256_set1_epi16(this->w_open);
    __m256i gapExtend256 = _mm256_set1_epi16(this->w_extend);
	__m256i gapOE256	 = _mm256_set1_epi16(this->w_open + this->w_extend);
	__m256i w_ambig_256	 = _mm256_set1_epi16(this->w_ambig);	// ambig penalty
	__m256i five256		 = _mm256_set1_epi16(5);

	__m256i e_del256	= _mm256_set1_epi16(this->e_del);
	__m256i oe_del256	= _mm256_set1_epi16(this->o_del + this->e_del);
	__m256i e_ins256	= _mm256_set1_epi16(this->e_ins);
	__m256i oe_ins256	= _mm256_set1_epi16(this->o_ins + this->e_ins);
	
    int16_t	*F	= F16_ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;
    int16_t	*H_h	= H16_ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;
	int16_t	*H_v = H16__ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;

	int lane = 0;
#if DEB
	lane = (spot - 1) % SIMD_WIDTH16;
#endif
	
    int16_t i, j;

	uint16_t tlen[SIMD_WIDTH16];
	uint16_t tail[SIMD_WIDTH16] __attribute((aligned(64)));
	uint16_t head[SIMD_WIDTH16] __attribute((aligned(64)));
	
	int minq = 1000;
	for (int l=0; l<SIMD_WIDTH16; l++) {
		tlen[l] = p[l].len1;
		qlen[l] = p[l].len2;
		if (p[l].len2 < minq) minq = p[l].len2;
	}
	minq -= 1; // for gscore

	__m256i tlen256 = _mm256_load_si256((__m256i *) tlen);
	__m256i qlen256 = _mm256_load_si256((__m256i *) qlen);
	__m256i myband256 = _mm256_load_si256((__m256i *) myband);
    __m256i zero256 = _mm256_setzero_si256();
	__m256i one256	= _mm256_set1_epi16(1);
	__m256i two256	= _mm256_set1_epi16(2);
	__m256i max_ie256 = zero256;
	__m256i ff256 = _mm256_set1_epi16(0xFFFF);
		
   	__m256i tail256 = qlen256, head256 = zero256;
	_mm256_store_si256((__m256i *) head, head256);
   	_mm256_store_si256((__m256i *) tail, tail256);
	//__m256i ib256 = _mm256_add_epi16(qlen256, qlen256);
	// ib256 = _mm256_sub_epi16(ib256, one256);

	__m256i mlen256 = _mm256_add_epi16(qlen256, myband256);
	mlen256 = _mm256_min_epu16(mlen256, tlen256);

	uint16_t temp[SIMD_WIDTH16]  __attribute((aligned(64)));
	uint16_t temp1[SIMD_WIDTH16]  __attribute((aligned(64)));
	
	__m256i s00	 = _mm256_load_si256((__m256i *)(seq1SoA));
	__m256i hval = _mm256_load_si256((__m256i *)(H_v));
	__mmask16 dmask = 0xFFFF;
	__mmask32 dmask32 = 0xAAAAAAAA;
		
	__m256i maxScore256 = hval;
    for(j = 0; j < ncol; j++)
		_mm256_store_si256((__m256i *)(F + j * SIMD_WIDTH16), zero256);
	
	__m256i x256 = zero256;
	__m256i y256 = zero256;
	__m256i i256 = zero256;
	__m256i gscore = _mm256_set1_epi16(-1);
	__m256i max_off256 = zero256;
	__m256i exit0 = _mm256_set1_epi16(0xFFFF);
	__m256i zdrop256 = _mm256_set1_epi16(zdrop);
	
	int beg = 0, end = ncol;
	int nbeg = beg, nend = end;

#if RDT
	uint64_t tim = _rdtsc();
#endif
	
    for(i = 0; i < nrow; i++)
    {		
        __m256i e11 = zero256;
        __m256i h00, h11, h10;
        __m256i s10 = _mm256_load_si256((__m256i *)(seq1SoA + (i + 0) * SIMD_WIDTH16));

		beg = nbeg; end = nend;
		int pbeg = beg;
		if (beg < i - w) beg = i - w;
		if (end > i + w + 1) end = i + w + 1;
		if (end > ncol) end = ncol;

		h10 = zero256;
		if (beg == 0)
			h10 = _mm256_load_si256((__m256i *)(H_v + (i+1) * SIMD_WIDTH16));

		__m256i j256 = zero256;
		__m256i maxRS1;
		maxRS1 = zero256;

		__m256i i1_256 = _mm256_set1_epi16(i+1);
		__m256i y1_256 = zero256;
		
#if RDT	
		uint64_t tim1 = _rdtsc();
#endif
		
		__m256i i256, cache256;
		__m256i phead256 = head256, ptail256 = tail256;
		i256 = _mm256_set1_epi16(i);
		cache256 = _mm256_sub_epi16(i256, myband256);
		head256 = _mm256_max_epi16(head256, cache256);
		cache256 = _mm256_add_epi16(i1_256, myband256);
		tail256 = _mm256_min_epu16(tail256, cache256);
		tail256 = _mm256_min_epu16(tail256, qlen256);

		// NEW, trimming.
		__m256i cmph = _mm256_cmpeq_epi16(head256, phead256);
		__m256i cmpt = _mm256_cmpeq_epi16(tail256, ptail256);
		// cmph &= cmpt;
		cmph = _mm256_and_si256(cmph, cmpt);
		//__mmask16 cmp_ht = _mm256_movepi16_mask(cmph);
		__mmask32 cmp_ht = _mm256_movemask_epi8(cmph) & dmask32;
		
		//for (int l=beg; l<end && cmp_ht != dmask; l++) {
		for (int l=beg; l<end && cmp_ht != dmask32; l++) {
			//for (int l=pbeg; l<beg; l++) {
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH16));
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH16));
			
			__m256i pj256 = _mm256_set1_epi16(l);
			__m256i j256 = _mm256_set1_epi16(l+1);
			__m256i cmp1 = _mm256_cmpgt_epi16(head256, pj256);
			//uint16_t cval = _mm256_movepi16_mask(cmp1);
			uint32_t cval = _mm256_movemask_epi8(cmp1) & dmask32;
			if (cval == 0x00) break;
			//__m256i cmp2 = _mm256_cmpgt_epi16(pj256, tail256);
			__m256i cmp2 = _mm256_cmpgt_epi16(j256, tail256);
			cmp1 = _mm256_or_si256(cmp1, cmp2);
			h256 = _mm256_blendv_epi16(h256, zero256, cmp1);
			f256 = _mm256_blendv_epi16(f256, zero256, cmp1);
			
			_mm256_store_si256((__m256i *)(F + l * SIMD_WIDTH16), f256);
			_mm256_store_si256((__m256i *)(H_h + l * SIMD_WIDTH16), h256);
		}


#if RDT
		prof[DP3][0] += _rdtsc() - tim1;
#endif

		// beg = nbeg; end = nend;
		__m256i cmp256_1 = _mm256_cmpgt_epi16(i1_256, tlen256);
		
		__m256i cmpim = _mm256_cmpgt_epi16(i1_256, mlen256);
		__m256i cmpht = _mm256_cmpeq_epi16(tail256, head256);
		cmpim = _mm256_or_si256(cmpim, cmpht);
		// change
#if NEW
		cmpht = _mm256_cmpgt_epi16(head256, tail256);
		cmpim = _mm256_or_si256(cmpim, cmpht);
#endif		
		exit0 = _mm256_blendv_epi16(exit0, zero256, cmpim);

		
#if RDT
		tim1 = _rdtsc();
#endif
		
		j256 = _mm256_set1_epi16(beg);
		for(j = beg; j < end; j++)
		{
            __m256i f11, f21, s2;
			h00 = _mm256_load_si256((__m256i *)(H_h + j * SIMD_WIDTH16));
            f11 = _mm256_load_si256((__m256i *)(F + j * SIMD_WIDTH16));

            s2 = _mm256_load_si256((__m256i *)(seq2SoA + (j) * SIMD_WIDTH16));
			
			__m256i pj256 = j256;
			j256 = _mm256_add_epi16(j256, one256);

            //MAIN_CODE16(s10, s2, h00, h11, e11, f11, f21, zero256,
			//			maxScore256, gapExtend256, y1_256, maxRS1); //i+1
			MAIN_CODE16(s10, s2, h00, h11, e11, f11, f21, zero256,
						maxScore256, e_ins256, oe_ins256,
						e_del256, oe_del256, gapExtend256,
						y1_256, maxRS1); //i+1
			
			// Masked writing
			__m256i cmp2 = _mm256_cmpgt_epi16(head256, pj256);
			__m256i cmp1 = _mm256_cmpgt_epi16(pj256, tail256);
			cmp1 = _mm256_or_si256(cmp1, cmp2);
			h10 = _mm256_blendv_epi16(h10, zero256, cmp1);
			f21 = _mm256_blendv_epi16(f21, zero256, cmp1);
			
			__m256i bmaxRS = maxRS1;										
			maxRS1 =_mm256_max_epi16(maxRS1, h11);							
			__m256i cmpA = _mm256_cmpgt_epi16(maxRS1, bmaxRS);					
			__m256i cmpB =_mm256_cmpeq_epi16(maxRS1, h11);					
			cmpA = _mm256_or_si256(cmpA, cmpB);
			cmp1 = _mm256_cmpgt_epi16(j256, tail256); // change
			cmp1 = _mm256_or_si256(cmp1, cmp2);			// change
			cmpA = _mm256_blendv_epi16(y1_256, j256, cmpA);
			y1_256 = _mm256_blendv_epi16(cmpA, y1_256, cmp1);
			maxRS1 = _mm256_blendv_epi16(maxRS1, bmaxRS, cmp1);						

			_mm256_store_si256((__m256i *)(F + j * SIMD_WIDTH16), f21);
			_mm256_store_si256((__m256i *)(H_h + j * SIMD_WIDTH16), h10);

			h10 = h11;
			
			//j256 = _mm256_add_epi16(j256, one256);
			
			// gscore calculations
			if (j >= minq)
			{
				__m256i cmp = _mm256_cmpeq_epi16(j256, qlen256);
				__m256i max_gh = _mm256_max_epi16(gscore, h11);
				__m256i cmp_gh = _mm256_cmpgt_epi16(gscore, h11);
				__m256i tmp256_1 = _mm256_blendv_epi16(i1_256, max_ie256, cmp_gh);

				__m256i tmp256_t = _mm256_blendv_epi16(max_ie256, tmp256_1, cmp);
				tmp256_1 = _mm256_blendv_epi16(max_ie256, tmp256_t, exit0);				

				max_gh = _mm256_blendv_epi16(gscore, max_gh, exit0);
				max_gh = _mm256_blendv_epi16(gscore, max_gh, cmp);				

				cmp = _mm256_cmpgt_epi16(j256, tail256); 
				max_gh = _mm256_blendv_epi16(max_gh, gscore, cmp);
				max_ie256 = _mm256_blendv_epi16(tmp256_1, max_ie256, cmp);
				gscore = max_gh;			
			}
        }
		_mm256_store_si256((__m256i *)(H_h + j * SIMD_WIDTH16), h10);
		_mm256_store_si256((__m256i *)(F + j * SIMD_WIDTH16), zero256);
				
		/* exit due to zero score by a row */
		// uint32_t cval = 0;
		__m256i bmaxScore256 = maxScore256;
		__m256i tmp = _mm256_cmpeq_epi16(maxRS1, zero256);
		// uint16_t cval = _mm256_movepi16_mask(tmp);
		uint32_t cval = _mm256_movemask_epi8(tmp) & dmask32;
		//if (cval == 0xFFFF) break;
		if (cval == dmask32) break;

		// _mm256_store_si256((__m256i *) temp, exit0);
		exit0 = _mm256_blendv_epi16(exit0, zero256,  tmp);
		// _mm256_store_si256((__m256i *) temp1, exit0);

		__m256i score256 = _mm256_max_epi16(maxScore256, maxRS1);
		maxScore256 = _mm256_blendv_epi16(maxScore256, score256, exit0);

		__m256i cmp = _mm256_cmpgt_epi16(maxScore256, bmaxScore256);
		y256 = _mm256_blendv_epi16(y256, y1_256, cmp);
		x256 = _mm256_blendv_epi16(x256, i1_256, cmp);		
		// max_off calculations
		tmp = _mm256_sub_epi16(y1_256, i1_256);
		tmp = _mm256_abs_epi16(tmp);
		__m256i bmax_off256 = max_off256;
		tmp = _mm256_max_epi16(max_off256, tmp);
		max_off256 = _mm256_blendv_epi16(bmax_off256, tmp, cmp);

		// Z-score
		ZSCORE16(i1_256, y1_256);		

#if RDT
		prof[DP1][0] += _rdtsc() - tim1;
		tim1 = _rdtsc();
#endif
		
		/* Narrowing of the band */
		/* From beg */
		int l;
 		for (l = beg; l < end; l++) {
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH16));
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH16));
			__m256i tmp = _mm256_or_si256(f256, h256);
			tmp = _mm256_cmpeq_epi16(tmp, zero256);
			//uint16_t val = _mm256_movepi16_mask(tmp);
			uint32_t val = _mm256_movemask_epi8(tmp) & dmask32;
			//if (val == 0xFFFF) nbeg = l;
			if (val == dmask32) nbeg = l;
			else
				break;
		}
		
		/* From end */
		bool flg = 1;
		for (l = end; l >= beg; l--) {
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH16));
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH16));
			__m256i tmp = _mm256_or_si256(f256, h256);
			tmp = _mm256_cmpeq_epi16(tmp, zero256);
			//uint16_t val = _mm256_movepi16_mask(tmp);
			uint32_t val = _mm256_movemask_epi8(tmp) & dmask32;
			//if (val != 0xFFFF && flg)
			if (val != dmask32 && flg)  
				break;
		}
		// int pnend =nend;
		nend = l + 2 < ncol? l + 2: ncol;

		__m256i tail256_ = _mm256_sub_epi16(tail256, one256);
		__m256i tmpb = ff256;

		__m256i exit1 = _mm256_xor_si256(exit0, ff256);
		__m256i l256 = _mm256_set1_epi16(beg);
		for (l = beg; l < end; l++) {
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH16));
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH16));
	
			__m256i tmp = _mm256_or_si256(f256, h256);
			tmp = _mm256_or_si256(tmp, exit1);			
			tmp = _mm256_cmpeq_epi16(tmp, zero256);
			//uint16_t val = _mm256_movepi16_mask(tmp);
			uint32_t val = _mm256_movemask_epi8(tmp) & dmask32;
			if (val == 0x00) {
				break;
			}
			tmp = _mm256_and_si256(tmp,tmpb);
			//__m256i l256 = _mm256_set1_epi16(l+1);
			l256 = _mm256_add_epi16(l256, one256);

#if !NEW
			__m256i mask2 = _mm256_cmpgt_epi16(l256, tail256_);
			mask2 = _mm256_blendv_epi16(l256, tail256,  mask2);
			head256 = _mm256_blendv_epi16(head256, mask2, tmp);
#else
			head256 = _mm256_blendv_epi16(head256, l256, tmp);
#endif
			tmpb = tmp;			
		}
		// _mm256_store_si256((__m256i *) head, head256);
		
		__m256i  index256 = tail256;
		tmpb = ff256;

		l256 = _mm256_set1_epi16(end);
		for (l = end; l >= beg; l--)
		{
			__m256i f256 = _mm256_load_si256((__m256i *)(F + l * SIMD_WIDTH16));
			__m256i h256 = _mm256_load_si256((__m256i *)(H_h + l * SIMD_WIDTH16));
			
			__m256i tmp = _mm256_or_si256(f256, h256);
			tmp = _mm256_or_si256(tmp, exit1);
			tmp = _mm256_cmpeq_epi16(tmp, zero256);			
			//uint16_t val = _mm256_movepi16_mask(tmp);
			uint32_t val = _mm256_movemask_epi8(tmp) & dmask32;
			if (val == 0x00)  {
				break;
			}
			tmp = _mm256_and_si256(tmp,tmpb);
			l256 = _mm256_sub_epi16(l256, one256);
#if !NEW
			__m256i mask2 = _mm256_cmpgt_epi16(l256, tail256);			
			mask2 = _mm256_blendv_epi16(l256, tail256,  mask2);
			index256 = _mm256_blendv_epi16(index256, mask2, tmp);
#else
			index256 = _mm256_blendv_epi8(index256, l256, tmp);
#endif
			tmpb = tmp;
		}
		index256 = _mm256_add_epi16(index256, two256);
		tail256 = _mm256_min_epi16(index256, qlen256);
		// _mm256_store_si256((__m256i *) tail, tail256);		

#if RDT
		prof[DP2][0] += _rdtsc() - tim1;
#endif
    }
	
#if RDT
	prof[DP][0] += _rdtsc() - tim;
#endif
	
    int16_t score[SIMD_WIDTH16]  __attribute((aligned(64)));
	_mm256_store_si256((__m256i *) score, maxScore256);

	int16_t maxi[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) maxi, x256);

	int16_t maxj[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) maxj, y256);

	int16_t max_off_ar[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) max_off_ar, max_off256);

	int16_t gscore_ar[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) gscore_ar, gscore);

	int16_t maxie_ar[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm256_store_si256((__m256i *) maxie_ar, max_ie256);
	
    for(i = 0; i < SIMD_WIDTH16; i++)
    {
		p[i].score = score[i];
		p[i].tle = maxi[i];
		p[i].qle = maxj[i];
		p[i].max_off = max_off_ar[i];
		p[i].gscore = gscore_ar[i];
		p[i].gtle = maxie_ar[i];
    }
	
    return;
}

#endif // AVX2

#if __AVX512BW__

// ----------------------------------------------------------------------------------
// AVX512- vec8, vec16 SIMD code
//
// ----------------------------------------------------------------------------------

// ------------------------ vec 8 --------------------------------------------------
#define MAX_INSDEL8(qlen512, eb512, band512, band512_)				\	
{																		\
	__m512i max_ins512 = _mm512_add_epi8(qlen512, eb_ins512);			\
	__mmask64 cmp512 = _mm512_cmpgt_epi8_mask(max_ins512, zero512);		\
	max_ins512 = _mm512_mask_blend_epi8(cmp512, zero512, max_ins512);	\
	cmp512 = _mm512_cmpgt_epi8_mask(max_ins512, band512_);				\
	max_ins512 = _mm512_mask_blend_epi8(cmp512, max_ins512, band512);	\
	_mm512_store_si512((__m512i *) qlen, max_ins512);				\		
}

#define ZSCORE8(i4_512, y4_512)											\
	{																	\
		__m512i tmpi = _mm512_sub_epi8(i4_512, x512);					\
		__m512i tmpj = _mm512_sub_epi8(y4_512, y512);					\
		cmp = _mm512_cmpgt_epi8_mask(tmpi, tmpj);						\
		score512 = _mm512_sub_epi8(maxScore512, maxRS1);				\
		__m512i insdel = _mm512_mask_blend_epi8(cmp, e_ins512, e_del512); \
		__m512i sub_a512 = _mm512_sub_epi8(tmpi, tmpj);					\
		__m512i sub_b512 = _mm512_sub_epi8(tmpj, tmpi);					\
		__m512i tmp1 = _mm512_mask_blend_epi8(cmp, sub_b512, sub_a512);			\
		tmp1 = _mm512_sub_epi8(score512, tmp1);							\
		cmp = _mm512_cmpgt_epi8_mask(tmp1, zdrop512);					\
		exit0 = _mm512_mask_blend_epi8(cmp, exit0, zero512);			\
	}

#define MAIN_CODE8_BAK(s1, s2, h00, h11, e11, f11, f21, zero512,  maxScore512, gapOpen512, gapExtend512, y512, maxRS) \
	{																	\
		__mmask64 cmp11 = _mm512_cmpeq_epi8_mask(s1, s2);				\
		__m512i sbt11 = _mm512_mask_blend_epi8(cmp11, mismatch512, match512); \
		__m512i tmp512 = _mm512_max_epu8(s1, s2);						\
		cmp11 = _mm512_movepi8_mask(tmp512);							\
		/*tmp512 = _mm512_cmpeq_epi8(tmp512, val102);*/					\
		sbt11 = _mm512_mask_blend_epi8(cmp11, sbt11, w_ambig_512);		\
		__m512i m11 = _mm512_add_epi8(h00, sbt11);						\
		cmp11 = _mm512_cmpeq_epi8_mask(h00, zero512);					\
		m11 = _mm512_mask_blend_epi8(cmp11, m11, zero512);				\
		h11 = _mm512_max_epi8(m11, e11);								\
		h11 = _mm512_max_epi8(h11, f11);								\
		__m512i temp512 = _mm512_sub_epi8(m11, gapOE512);				\
		__m512i val512  = _mm512_max_epi8(temp512, zero512);			\
		e11 = _mm512_sub_epi8(e11, gapExtend512);						\
		e11 = _mm512_max_epi8(val512, e11);								\
		f21 = _mm512_sub_epi8(f11, gapExtend512);						\
		f21 = _mm512_max_epi8(val512, f21);								\
	}

#define MAIN_CODE8(s1, s2, h00, h11, e11, f11, f21, zero512,  maxScore512, e_ins512, oe_ins512, e_del512, oe_del512, gapExtend512, y512, maxRS) \
	{																	\
		__mmask64 cmp11;												\
		__m512i sbt11, tmp512, m11, temp512, val512;					\
		cmp11 = _mm512_cmpeq_epi8_mask(s1, s2);							\
		sbt11 = _mm512_mask_blend_epi8(cmp11, mismatch512, match512);	\
		tmp512 = _mm512_max_epu8(s1, s2);								\
		cmp11 = _mm512_movepi8_mask(tmp512);							\
		/*tmp512 = _mm512_cmpeq_epi8(tmp512, val102);*/					\
		sbt11 = _mm512_mask_blend_epi8(cmp11, sbt11, w_ambig_512);		\
		m11 = _mm512_add_epi8(h00, sbt11);								\
		cmp11 = _mm512_cmpeq_epi8_mask(h00, zero512);					\
		m11 = _mm512_mask_blend_epi8(cmp11, m11, zero512);				\
		h11 = _mm512_max_epi8(m11, e11);								\
		h11 = _mm512_max_epi8(h11, f11);								\
		temp512 = _mm512_sub_epi8(m11, oe_ins512);						\
		val512  = _mm512_max_epi8(temp512, zero512);					\
		e11 = _mm512_sub_epi8(e11, e_ins512);							\
		e11 = _mm512_max_epi8(val512, e11);								\
		temp512 = _mm512_sub_epi8(m11, oe_del512);						\
		val512  = _mm512_max_epi8(temp512, zero512);					\
		f21 = _mm512_sub_epi8(f11, e_del512);							\
		f21 = _mm512_max_epi8(val512, f21);								\
	}

// ------------------------ vec 16 --------------------------------------------------
#define MAX_INSDEL16(qlen512, eb512, band512, band512_)					\
	{																	\
	__m512i max_ins512 = _mm512_add_epi16(qlen512, eb_ins512);			\
	__mmask32 cmp512 = _mm512_cmpgt_epi16_mask(max_ins512, zero512);	\
	max_ins512 = _mm512_mask_blend_epi16(cmp512, zero512, max_ins512);	\
	cmp512 = _mm512_cmpgt_epi16_mask(max_ins512, band512_);				\
	max_ins512 = _mm512_mask_blend_epi16(cmp512, max_ins512, band512);	\
	_mm512_store_si512((__m512i *) qlen, max_ins512);\		
}

#define ZSCORE16(i4_512, y4_512)											\
	{																	\
		__m512i tmpi = _mm512_sub_epi16(i4_512, x512);					\
		__m512i tmpj = _mm512_sub_epi16(y4_512, y512);					\
		cmp = _mm512_cmpgt_epi16_mask(tmpi, tmpj);						\
		score512 = _mm512_sub_epi16(maxScore512, maxRS1);				\
		__m512i insdel = _mm512_mask_blend_epi16(cmp, e_ins512, e_del512); \
		__m512i sub_a512 = _mm512_sub_epi16(tmpi, tmpj);					\
		__m512i sub_b512 = _mm512_sub_epi16(tmpj, tmpi);					\
		__m512i tmp1 = _mm512_mask_blend_epi16(cmp, sub_b512, sub_a512);			\
		tmp1 = _mm512_sub_epi16(score512, tmp1);							\
		cmp = _mm512_cmpgt_epi16_mask(tmp1, zdrop512);					\
		exit0 = _mm512_mask_blend_epi16(cmp, exit0, zero512);			\
	}

#define MAIN_CODE16_BAK(s1, s2, h00, h11, e11, f11, f21, zero512,  maxScore512, gapOpen512, gapExtend512, y512, maxRS) \
	{																	\
		__mmask32 cmp11 = _mm512_cmpeq_epi16_mask(s1, s2);				\
		__m512i sbt11 = _mm512_mask_blend_epi16(cmp11, mismatch512, match512); \
		__m512i tmp512 = _mm512_max_epu16(s1, s2);						\
		cmp11 = _mm512_movepi16_mask(tmp512);							\
		/*tmp512 = _mm512_cmpeq_epi16(tmp512, val102);*/					\
		sbt11 = _mm512_mask_blend_epi16(cmp11, sbt11, w_ambig_512);		\
		__m512i m11 = _mm512_add_epi16(h00, sbt11);						\
		cmp11 = _mm512_cmpeq_epi16_mask(h00, zero512);					\
		m11 = _mm512_mask_blend_epi16(cmp11, m11, zero512);				\
		h11 = _mm512_max_epi16(m11, e11);								\
		h11 = _mm512_max_epi16(h11, f11);								\
		__m512i temp512 = _mm512_sub_epi16(m11, gapOE512);				\
		__m512i val512  = _mm512_max_epi16(temp512, zero512);			\
		e11 = _mm512_sub_epi16(e11, gapExtend512);						\
		e11 = _mm512_max_epi16(val512, e11);								\
		f21 = _mm512_sub_epi16(f11, gapExtend512);						\
		f21 = _mm512_max_epi16(val512, f21);								\
	}

#define MAIN_CODE16(s1, s2, h00, h11, e11, f11, f21, zero512,  maxScore512, e_ins512, oe_ins512, e_del512, oe_del512, gapExtend512, y512, maxRS) \
	{																	\
		__mmask32 cmp11;												\
		__m512i sbt11, tmp512, m11, temp512, val512;					\
		cmp11 = _mm512_cmpeq_epi16_mask(s1, s2);						\
		sbt11 = _mm512_mask_blend_epi16(cmp11, mismatch512, match512);	\
		tmp512 = _mm512_max_epu16(s1, s2);								\
		cmp11 = _mm512_movepi16_mask(tmp512);							\
		/*tmp512 = _mm512_cmpeq_epi16(tmp512, val102);*/				\
		sbt11 = _mm512_mask_blend_epi16(cmp11, sbt11, w_ambig_512);		\
		m11 = _mm512_add_epi16(h00, sbt11);								\
		cmp11 = _mm512_cmpeq_epi16_mask(h00, zero512);					\
		m11 = _mm512_mask_blend_epi16(cmp11, m11, zero512);				\
		h11 = _mm512_max_epi16(m11, e11);								\
		h11 = _mm512_max_epi16(h11, f11);								\
		temp512 = _mm512_sub_epi16(m11, oe_ins512);						\
		val512  = _mm512_max_epi16(temp512, zero512);					\
		e11 = _mm512_sub_epi16(e11, e_ins512);							\
		e11 = _mm512_max_epi16(val512, e11);							\
		temp512 = _mm512_sub_epi16(m11, oe_del512);						\
		val512  = _mm512_max_epi16(temp512, zero512);					\
		f21 = _mm512_sub_epi16(f11, e_del512);							\
		f21 = _mm512_max_epi16(val512, f21);							\
	}


inline void sortPairsLen(SeqPair *pairArray, int32_t count, SeqPair *tempArray, int16_t *hist, int16_t *histb)
{

    int32_t i;
    __m512i zero512 = _mm512_setzero_si512();
    for(i = 0; i <= MAX_SEQ_LEN8; i+=32)
    {
        _mm512_store_si512((__m512i *)(hist + i), zero512);
		_mm512_store_si512((__m512i *)(histb + i), zero512);
    }

    
    for(i = 0; i < count; i++)
    {
        SeqPair sp = pairArray[i];
        hist[sp.len1]++;
		// histb[sp.len1]++;
    }

    int32_t prev = 0;
    int32_t cumulSum = 0;
    for(i = 0; i <= MAX_SEQ_LEN8; i++)
    {
        int32_t cur = hist[i];
        hist[i] = cumulSum;
		// histb[i] = cumulSum;
        cumulSum += cur;
    }

    for(i = 0; i < count; i++)
    {
        SeqPair sp = pairArray[i];
        int32_t pos = hist[sp.len1];

        tempArray[pos] = sp;
        hist[sp.len1]++;
    }

    for(i = 0; i < count; i++) {
        pairArray[i] = tempArray[i];
	}
}

inline void sortPairsId(SeqPair *pairArray, int32_t first, int32_t count,
						SeqPair *tempArray)
{

    int32_t i;
    
    for(i = 0; i < count; i++)
    {
        SeqPair sp = pairArray[i];
        int32_t pos = sp.id - first;
        tempArray[pos] = sp;
    }

    for(i = 0; i < count; i++)
        pairArray[i] = tempArray[i];
}

// ____________________________ AVX512 - getScore() _______________________________________
#define PFD8 5
void BandedPairWiseSW::getScores8(SeqPair *pairArray,
								  uint8_t *seqBufRef,
								  uint8_t *seqBufQer,
								  int32_t numPairs,
								  uint16_t numThreads,
								  int8_t w)
{
	assert(SIMD_WIDTH8 == 64 && SIMD_WIDTH16 == 32);
    int i;
    int64_t startTick, endTick;
	// F8_ = H8_ = H8__ = NULL;
    // F8_ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);
    // H8_ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);
	// H8__ = (int8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(int8_t), 64);
	// if (F8_ == NULL || H8_ == NULL || H8__ == NULL) {
	// 	printf("Memory not alloacted!!!\n");
	// 	exit(0);
	// }   

	smithWatermanBatchWrapper8(pairArray, seqBufRef, seqBufQer, numPairs, numThreads, w);

	// _mm_free(F8_);	_mm_free(H8_);	_mm_free(H8__);
	
#if MAXI
	printf("AVX512/8 Vecor code: Writing output..\n");
	for (int l=0; l<numPairs; l++)
	{
		fprintf(stderr, "%d (%d %d) %d %d %d\n",
				pairArray[l].score, pairArray[l].tle, pairArray[l].qle,
				pairArray[l].gscore, pairArray[l].max_off, pairArray[l].gtle);

	}
	printf("Vector code: Writing output completed!!!\n\n");
#endif

}

void BandedPairWiseSW::smithWatermanBatchWrapper8(SeqPair *pairArray,
												  uint8_t *seqBufRef,
												  uint8_t *seqBufQer,
												  int32_t numPairs,
												  uint16_t numThreads,
												  int8_t w)
{
	// printf("numThreads: %d\n", numThreads);
	// assert(numThreads == 1 && SIMD_WIDTH8 == 64);
	int64_t st1, st2, st3, st4, st5;
    // st1 = __rdtsc();
    uint8_t *seq1SoA = (uint8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(uint8_t), 64);
    uint8_t *seq2SoA = (uint8_t *)_mm_malloc(MAX_SEQ_LEN8 * SIMD_WIDTH8 * numThreads * sizeof(uint8_t), 64);

    int32_t ii;
    int32_t roundNumPairs = ((numPairs + SIMD_WIDTH8 - 1)/SIMD_WIDTH8 ) * SIMD_WIDTH8;
    for(ii = numPairs; ii < roundNumPairs; ii++)
    {
        pairArray[ii].id = ii;
        pairArray[ii].len1 = 0;
        pairArray[ii].len2 = pairArray[numPairs - 1].len2;
    }

    // st2 = __rdtsc();	
#if SORT_PAIRS
    // Sort the sequences according to decreasing order of lengths
    SeqPair *tempArray = (SeqPair *)_mm_malloc(SORT_BLOCK_SIZE * numThreads *
											   sizeof(SeqPair), 64);
    int16_t *hist = (int16_t *)_mm_malloc((MAX_SEQ_LEN8 + 32) * numThreads *
										  sizeof(int16_t), 64);
	int16_t *histb = (int16_t *)_mm_malloc((MAX_SEQ_LEN8 + 32) * numThreads *
										   sizeof(int16_t), 64);
#pragma omp parallel num_threads(numThreads)
    {
        int32_t tid = omp_get_thread_num();
        SeqPair *myTempArray = tempArray + tid * SORT_BLOCK_SIZE;
        int16_t *myHist = hist + tid * (MAX_SEQ_LEN8 + 32);
		int16_t *myHistb = histb + tid * (MAX_SEQ_LEN8 + 32);

#pragma omp for
        for(ii = 0; ii < roundNumPairs; ii+=SORT_BLOCK_SIZE)
        {
            int32_t first, last;
            first = ii;
            last  = ii + SORT_BLOCK_SIZE;
            if(last > roundNumPairs) last = roundNumPairs;
            sortPairsLen(pairArray + first, last - first, myTempArray, myHist, myHistb);
        }
    }
    _mm_free(hist);
#endif
    // st3 = __rdtsc();

#ifdef VTUNE_ANALYSIS
    __itt_resume();
#endif

	int eb = end_bonus;
#pragma omp parallel num_threads(numThreads)
    {
        // int64_t st = __rdtsc();
        int32_t i;
        uint16_t tid = omp_get_thread_num();
        uint8_t *mySeq1SoA = seq1SoA + tid * MAX_SEQ_LEN8 * SIMD_WIDTH8;
        uint8_t *mySeq2SoA = seq2SoA + tid * MAX_SEQ_LEN8 * SIMD_WIDTH8;
        uint8_t *seq1;
        uint8_t *seq2;
		uint8_t h0[SIMD_WIDTH8];
		uint8_t band[SIMD_WIDTH8];		
		uint8_t qlen[SIMD_WIDTH8] __attribute__((aligned(64)));
		int8_t bsize = 0;
		
		int8_t *H1 = H8_ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
		//int8_t *H2 = H8__ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
		int8_t *H2 = H8__ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN8;

		__m512i zero512	  = _mm512_setzero_si512();
		__m512i o_ins512  = _mm512_set1_epi8(o_ins);
		__m512i e_ins512  = _mm512_set1_epi8(e_ins);
		__m512i oe_ins512 = _mm512_set1_epi8(o_ins + e_ins);
		__m512i o_del512  = _mm512_set1_epi8(o_del);
		__m512i e_del512  = _mm512_set1_epi8(e_del);
		__m512i eb_ins512 = _mm512_set1_epi8(eb - o_ins);
		__m512i eb_del512 = _mm512_set1_epi8(eb - o_del);
		
		int8_t max = 0;
		if (max < w_match) max = w_match;
		if (max < w_mismatch) max = w_mismatch;
		if (max < w_ambig) max = w_ambig;
		
		int nstart = 0, nend = numPairs;

#if DEB
		int div = (spot-1) / SIMD_WIDTH8;
		nstart = div * SIMD_WIDTH8;
		nend = (div+1) * SIMD_WIDTH8;
		int lane = (spot - 1) % SIMD_WIDTH8;
		printf("nstart: %d, nend: %d\n", nstart, nend);
#endif
		
//#pragma omp for schedule(dynamic, 128)
		for(i = nstart; i < nend; i+=SIMD_WIDTH8)
		{
            int32_t j, k;
            // uint8_t maxLen1 = 0;
			uint16_t maxLen1 = 0;
            uint8_t maxLen2 = 0;
            uint8_t minLen1 = MAX_SEQ_LEN8 + 1;
            uint8_t minLen2 = MAX_SEQ_LEN8 + 1;
			//bsize = 100;
			bsize = w;
			
			uint64_t tim;
			// tim = _rdtsc();

            for(j = 0; j < SIMD_WIDTH8; j++)
            {
				{ // prefetch block
					SeqPair spf = pairArray[i + j + PFD8];
					//_mm_prefetch((const char*) seqBufRef + (int64_t)spf.id * MAX_SEQ_LEN_REF, 0);
					//_mm_prefetch((const char*) seqBufRef + (int64_t)spf.id * MAX_SEQ_LEN_REF + 64, 0);
					_mm_prefetch((const char*) seqBufRef + (int64_t)spf.idr, 0);
					_mm_prefetch((const char*) seqBufRef + (int64_t)spf.idr + 64, 0);
				}
                SeqPair sp = pairArray[i + j];
				h0[j] = sp.h0;
				seq1 = seqBufRef + (int64_t)sp.idr;

				// assert(sp.len1 < MAX_SEQ_LEN8);
                for(k = 0; k < sp.len1; k++)
                {
                    mySeq1SoA[k * SIMD_WIDTH8 + j] = (seq1[k] == AMBIG?0xFF:seq1[k]);
					H2[k * SIMD_WIDTH8 + j] = 0;
                }
				qlen[j] = sp.len2 * max;
                if(maxLen1 < sp.len1) maxLen1 = sp.len1;
            }
			// prof[1][tid] += _rdtsc() - tim;

            for(j = 0; j < SIMD_WIDTH8; j++)
            {
                SeqPair sp = pairArray[i + j];
                for(k = sp.len1; k <= maxLen1; k++)
                {
                    mySeq1SoA[k * SIMD_WIDTH8 + j] = DUMMY1;
					H2[k * SIMD_WIDTH8 + j] = DUMMY1;
                }
            }
//--------------------
			__m512i h0_512 = _mm512_load_si512((__m512i*) h0);
			_mm512_store_si512((__m512i *) H2, h0_512);
			__m512i tmp512 = _mm512_sub_epi8(h0_512, o_del512);
			
			for(k = 1; k < maxLen1; k++) {
				tmp512 = _mm512_sub_epi8(tmp512, e_del512);
				__m512i tmp512_ = _mm512_max_epi8(tmp512, zero512);
				_mm512_store_si512((__m512i *)(H2 + k* SIMD_WIDTH8), tmp512_);
			}
//-------------------
			// tim = _rdtsc();
            for(j = 0; j < SIMD_WIDTH8; j++)
            {
				{ // prefetch block
					SeqPair spf = pairArray[i + j + PFD8];
					//_mm_prefetch((const char*) seqBufQer + (int64_t)spf.id * MAX_SEQ_LEN_QER, 0);
					//_mm_prefetch((const char*) seqBufQer + (int64_t)spf.id * MAX_SEQ_LEN_QER + 64, 0);
					_mm_prefetch((const char*) seqBufQer + (int64_t)spf.idq, 0);
					_mm_prefetch((const char*) seqBufQer + (int64_t)spf.idq + 64, 0);
				}
				
                SeqPair sp = pairArray[i + j];
                // seq2 = seqBufQer + (int64_t)sp.id * MAX_SEQ_LEN_QER;
				seq2 = seqBufQer + (int64_t)sp.idq;
                for(k = 0; k < sp.len2; k++)
                {
                    mySeq2SoA[k * SIMD_WIDTH8 + j] = (seq2[k]==AMBIG?0xFF:seq2[k]);
					H1[k * SIMD_WIDTH8 + j] = 0;					
                }
                if(maxLen2 < sp.len2) maxLen2 = sp.len2;
            }
			// prof[1][tid] += _rdtsc() - tim;
			
            for(j = 0; j < SIMD_WIDTH8; j++)
            {
                SeqPair sp = pairArray[i + j];
                for(k = sp.len2; k <= maxLen2; k++)
                {
                    mySeq2SoA[k * SIMD_WIDTH8 + j] = DUMMY2;
					H1[k * SIMD_WIDTH8 + j] = 0;
                }
            }
//------------------------
			_mm512_store_si512((__m512i *) H1, h0_512);
			__mmask64 mask512 = _mm512_cmpgt_epi8_mask(h0_512, oe_ins512);
			tmp512 = _mm512_sub_epi8(h0_512, oe_ins512);
			tmp512 = _mm512_mask_blend_epi8(mask512, zero512, tmp512);
			_mm512_store_si512((__m512i *) (H1 + SIMD_WIDTH8), tmp512);

			for(k = 2; k < maxLen2; k++)
			{
				__m512i h1_512 = tmp512;
				tmp512 = _mm512_sub_epi8(h1_512, e_ins512);
				tmp512 = _mm512_max_epi8(tmp512, zero512);
				_mm512_store_si512((__m512i *)(H1 + k*SIMD_WIDTH8), tmp512);
            }			
//------------------------
			/* Banding calculation in pre-processing */
#if 1
			uint8_t myband[SIMD_WIDTH8] __attribute__((aligned(64)));
			uint8_t temp[SIMD_WIDTH8] __attribute__((aligned(64)));
			{
				__m512i qlen512 = _mm512_load_si512((__m512i *) qlen);
				__m512i sum512 = _mm512_add_epi8(qlen512, eb_ins512);
				_mm512_store_si512((__m512i *) temp, sum512);				
				for (int l=0; l<SIMD_WIDTH8; l++) {
					double val = temp[l]/e_ins + 1.0;
					int max_ins = val;
					max_ins = max_ins > 1? max_ins : 1;
					myband[l] = min(bsize, max_ins);
				}
				sum512 = _mm512_add_epi8(qlen512, eb_del512);
				_mm512_store_si512((__m512i *) temp, sum512);				
				for (int l=0; l<SIMD_WIDTH8; l++) {
					double val = temp[l]/e_del + 1.0;
					int max_ins = val;
					max_ins = max_ins > 1? max_ins : 1;
					myband[l] = min(myband[l], max_ins);
					bsize = bsize < myband[l] ? myband[l] : bsize;
				}
			}
#endif

			// uint64_t tim_swa = _rdtsc();
            smithWaterman512_8(mySeq1SoA,
							   mySeq2SoA,
							   maxLen1,
							   maxLen2,
							   pairArray + i,
							   h0,
							   tid,
							   numPairs,
							   zdrop,
							   bsize,
							   qlen,
							   myband);
			// prof[0][tid] += _rdtsc() - tim_swa;
        }
    }

#ifdef VTUNE_ANALYSIS
    __itt_pause();
#endif
	
    // st4 = __rdtsc();
#if SORT_PAIRS
	{
    // Sort the sequences according to increasing order of id
#pragma omp parallel num_threads(numThreads)
    {
        int32_t tid = omp_get_thread_num();
        SeqPair *myTempArray = tempArray + tid * SORT_BLOCK_SIZE;

#pragma omp for
        for(ii = 0; ii < roundNumPairs; ii+=SORT_BLOCK_SIZE)
        {
            int32_t first, last;
            first = ii;
            last  = ii + SORT_BLOCK_SIZE;
            if(last > roundNumPairs) last = roundNumPairs;
            sortPairsId(pairArray + first, first, last - first, myTempArray);
        }
    }
    _mm_free(tempArray);
	}
#endif

    // st5 = __rdtsc();
    setupTicks = st2 - st1;
    sort1Ticks = st3 - st2;
    swTicks = st4 - st3;
    sort2Ticks = st5 - st4;

	// free mem
	_mm_free(seq1SoA);
	_mm_free(seq2SoA);

    return;
}

void BandedPairWiseSW::smithWaterman512_8(uint8_t seq1SoA[],
										  uint8_t seq2SoA[],
										  uint8_t nrow,
										  uint8_t ncol,
										  SeqPair *p,
										  uint8_t h0[],
										  uint16_t tid,
										  int32_t numPairs,
										  int zdrop,
										  uint8_t w,
										  uint8_t qlen[],
										  uint8_t myband[])
{
#if DEB
	int8_t Hmat[nrow + 10][ncol + 10], Fmat[nrow + 10][ncol + 10];;
#endif
	
	static int cntr = 0;

    __m512i match512	 = _mm512_set1_epi8(this->w_match);
    __m512i mismatch512	 = _mm512_set1_epi8(this->w_mismatch);
    __m512i gapOpen512	 = _mm512_set1_epi8(this->w_open);
    __m512i gapExtend512 = _mm512_set1_epi8(this->w_extend);
	__m512i gapOE512	 = _mm512_set1_epi8(this->w_open + this->w_extend);
	__m512i w_ambig_512	 = _mm512_set1_epi8(this->w_ambig);	// ambig penalty
	__m512i five512		 = _mm512_set1_epi8(5);

	__m512i e_del512	= _mm512_set1_epi8(this->e_del);
	__m512i oe_del512	= _mm512_set1_epi8(this->o_del + this->e_del);
	__m512i e_ins512	= _mm512_set1_epi8(this->e_ins);
	__m512i oe_ins512	= _mm512_set1_epi8(this->o_ins + this->e_ins);	
	
	int8_t	*F	 = F8_ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
    int8_t	*H_h = H8_ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
	int8_t	*H_v = H8__ + tid * SIMD_WIDTH8 * MAX_SEQ_LEN8;
	
	int lane = 0;
#if DEB
	lane = (spot - 1)%32;
#endif
	
    int8_t lowInitValue = LOW_INIT_VALUE;
    int16_t i, j;

	uint8_t tlen[SIMD_WIDTH8];
	uint8_t tail[SIMD_WIDTH8] __attribute((aligned(64)));
	uint8_t head[SIMD_WIDTH8] __attribute((aligned(64)));
	
	int minq = 1000;
	for (int l=0; l<SIMD_WIDTH8; l++) {
		tlen[l] = p[l].len1;
		qlen[l] = p[l].len2;
		if (p[l].len2 < minq) minq = p[l].len2;
	}
	minq -= 1; // for gscore

	__m512i tlen512	  = _mm512_load_si512((__m512i *) tlen);
	__m512i qlen512	  = _mm512_load_si512((__m512i *) qlen);
	__m512i myband512 = _mm512_load_si512((__m512i *) myband);
    __m512i zero512	  = _mm512_setzero_si512();
	__m512i one512	  = _mm512_set1_epi8(1);
	__m512i two512	  = _mm512_set1_epi8(2);
	__m512i i512_1	  = _mm512_set1_epi8(1);
	__m512i max_ie512 = zero512;
	__m512i ff512	  = _mm512_set1_epi8(0xFF);
	
   	__m512i tail512 = qlen512, head512 = zero512;
	_mm512_store_si512((__m512i *) head, head512);
   	_mm512_store_si512((__m512i *) tail, tail512);
	//__m512i ib512 = _mm512_add_epi8(qlen512, qlen512);
	// ib512 = _mm512_sub_epi8(ib512, one512);

	__m512i mlen512 = _mm512_add_epi8(qlen512, myband512);
	mlen512 = _mm512_min_epu8(mlen512, tlen512);
	
	uint8_t temp[SIMD_WIDTH8]  __attribute((aligned(64)));
	uint8_t temp1[SIMD_WIDTH8]  __attribute((aligned(64)));
	uint8_t temp2[SIMD_WIDTH8]  __attribute((aligned(64)));
	uint8_t temp3[SIMD_WIDTH8]  __attribute((aligned(64)));
	
	__m512i s00	 = _mm512_load_si512((__m512i *)(seq1SoA));
	__m512i hval = _mm512_load_si512((__m512i *)(H_v));
	__mmask64 dmask = 0xFFFFFFFFFFFFFFFF;
	
///////
	__m512i maxScore512 = hval;
    for(j = 0; j < ncol; j++)
		_mm512_store_si512((__m512i *)(F + j * SIMD_WIDTH8), zero512);
	
	__m512i x512	   = zero512;
	__m512i y512	   = zero512;
	__m512i i512	   = zero512;
	__m512i gscore	   = _mm512_set1_epi8(-1);
	__m512i max_off512 = zero512;
	__m512i exit0	   = _mm512_set1_epi8(0xFF);
	__m512i zdrop512   = _mm512_set1_epi8(zdrop);

	int beg = 0, end = ncol;
	int nbeg = beg, nend = end;

#if RDT
	uint64_t tim = _rdtsc();
#endif

    for(i = 0; i < nrow; i++)
    {
        __m512i e11 = zero512;
        __m512i h00, h11, h10;
        __m512i s10 = _mm512_load_si512((__m512i *)(seq1SoA + (i + 0) * SIMD_WIDTH8));

		beg = nbeg; end = nend;
		int pbeg = beg;
		if (beg < i - w) beg = i - w;
		if (end > i + w + 1) end = i + w + 1;
		if (end > ncol) end = ncol;

		h10 = zero512;
		if (beg == 0)
			h10 = _mm512_load_si512((__m512i *)(H_v + (i+1) * SIMD_WIDTH8));

		__m512i j512 = zero512;
		__m512i maxRS1, maxRS2, maxRS3, maxRS4;
		maxRS1 = zero512;

		__m512i i1_512 = _mm512_set1_epi8(i+1);
		__m512i y1_512 = zero512;
		
#if RDT	
		uint64_t tim1 = _rdtsc();
#endif
		
		/* Banding */
		__m512i i512, cache512, max512;
		__m512i phead512 = head512, ptail512 = tail512;
		i512 = _mm512_set1_epi8(i);
		cache512 = _mm512_sub_epi8(i512, myband512);
		head512	 = _mm512_max_epi8(head512, cache512);
		cache512 = _mm512_add_epi8(i1_512, myband512);
		tail512	 = _mm512_min_epu8(tail512, cache512);
		tail512	 = _mm512_min_epu8(tail512, qlen512);
     	/* Banding ends */
		
		// NEW, trimming.
		__mmask64 cmph = _mm512_cmpeq_epi8_mask(head512, phead512);
		__mmask64 cmpt = _mm512_cmpeq_epi8_mask(tail512, ptail512);
		cmph &= cmpt;
		
		for (int l=beg; l<end && cmph != dmask; l++) {
			// for (int l=beg; l<end; l++) {
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH8));
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH8));

			__m512i pj512 = _mm512_set1_epi8(l);
			__m512i j512 = _mm512_set1_epi8(l+1);
			__mmask64 cmp1 = _mm512_cmpgt_epi8_mask(head512, pj512);
			if (cmp1 == 0x00) break;
			//__mmask64 cmp2 = _mm512_cmpgt_epi8_mask(pj512, tail512);
			__mmask64 cmp2 = _mm512_cmpgt_epi8_mask(j512, tail512);
			cmp1 = cmp1 | cmp2;
			h512 = _mm512_mask_blend_epi8(cmp1, h512, zero512);
			f512 = _mm512_mask_blend_epi8(cmp1, f512, zero512);

			_mm512_store_si512((__m512i *)(F + l * SIMD_WIDTH8), f512);
			_mm512_store_si512((__m512i *)(H_h + l * SIMD_WIDTH8), h512);
		}

#if RDT
		prof[DP3][0] += _rdtsc() - tim1;
#endif

#if DEB
		_mm512_store_si512((__m512i *) head, head512);
		_mm512_store_si512((__m512i *) tail, tail512);
		
		for (int l=0; l<=ncol; l++)
			Hmat[i][l] = H_h[l * SIMD_WIDTH8 + lane];
		
		_mm512_store_si512((__m512i *) temp, exit0);
		if (temp[lane]) {
			printf("%d, h: %d, t: %d, myband: %d, beg: %d, end: %d, exit: %d\n",
				   i, head[lane], tail[lane], myband[lane], beg, end, temp[lane]);
		}		
#endif
		// beg = nbeg; end = nend;
		__mmask64 cmp512_1 = _mm512_cmpgt_epi8_mask(i1_512, tlen512);

		/* Updating row exit status */
		__mmask64 cmpim = _mm512_cmpgt_epi8_mask(i1_512, mlen512);
		__mmask64 cmpht = _mm512_cmpeq_epi8_mask(tail512, head512);
		cmpim = cmpim | cmpht;
#if NEW
		cmpht = _mm512_cmpgt_epi8_mask(head512, tail512);
		cmpim = cmpim |  cmpht;
#endif
		exit0 = _mm512_mask_blend_epi8(cmpim, exit0, zero512);

		
#if RDT
		tim1 = _rdtsc();
#endif
		
		j512 = _mm512_set1_epi8(beg);
		for(j = beg; j < end; j++)
		{
            __m512i f11, f21, f31, f41, f51, jj512, s2;
			h00 = _mm512_load_si512((__m512i *)(H_h + j * SIMD_WIDTH8));
            f11 = _mm512_load_si512((__m512i *)(F + j * SIMD_WIDTH8));

            s2 = _mm512_load_si512((__m512i *)(seq2SoA + (j) * SIMD_WIDTH8));
			
			__m512i pj512 = j512;
			j512 = _mm512_add_epi8(j512, one512);
			
            //MAIN_CODE8(s10, s2, h00, h11, e11, f11, f21, zero512,
			//		  maxScore512, gapOpen512, gapExtend512, y1_512, maxRS1); //i+1
			MAIN_CODE8(s10, s2, h00, h11, e11, f11, f21, zero512,
					   maxScore512, e_ins512, oe_ins512,
					   e_del512, oe_del512, gapExtend512,
					   y1_512, maxRS1); //i+1

			// Masked writing
			__mmask64 cmp2 = _mm512_cmpgt_epi8_mask(head512, pj512);
			__mmask64 cmp1 = _mm512_cmpgt_epi8_mask(pj512, tail512);
			cmp1 = cmp1 | cmp2;
			h10 = _mm512_mask_blend_epi8(cmp1, h10, zero512);
			f21 = _mm512_mask_blend_epi8(cmp1, f21, zero512);
			
			/* Part of main code MAIN_CODE */
			__m512i bmaxRS = maxRS1, blend512;										
			maxRS1 =_mm512_max_epi8(maxRS1, h11);							
			__mmask64 cmpA = _mm512_cmpgt_epi8_mask(maxRS1, bmaxRS);					
			__mmask64 cmpB =_mm512_cmpeq_epi8_mask(maxRS1, h11);					
			cmpA = cmpA | cmpB;
			cmp1 = _mm512_cmpgt_epi8_mask(j512, tail512);
			cmp1 = cmp1 | cmp2;
			blend512 = _mm512_mask_blend_epi8(cmpA, y1_512, j512);
			y1_512 = _mm512_mask_blend_epi8(cmp1, blend512, y1_512);
			maxRS1 = _mm512_mask_blend_epi8(cmp1, maxRS1, bmaxRS);						

			_mm512_store_si512((__m512i *)(F + j * SIMD_WIDTH8), f21);
			_mm512_store_si512((__m512i *)(H_h + j * SIMD_WIDTH8), h10);

			h10 = h11;
						
			/* gscore calculations */
			if (j >= minq)
			{
				__mmask64 cmp = _mm512_cmpeq_epi8_mask(j512, qlen512);
				__m512i max_gh = _mm512_max_epi8(gscore, h11);
				__mmask64 cmp_gh = _mm512_cmpgt_epi8_mask(gscore, h11);
				__m512i tmp512_1 = _mm512_mask_blend_epi8(cmp_gh, i1_512, max_ie512);

				tmp512_1 = _mm512_mask_blend_epi8(cmp, max_ie512, tmp512_1);
				__mmask64 mex0 = _mm512_movepi8_mask(exit0);
				tmp512_1 = _mm512_mask_blend_epi8(mex0, max_ie512, tmp512_1);
				
				max_gh = _mm512_mask_blend_epi8(mex0, gscore, max_gh);
				max_gh = _mm512_mask_blend_epi8(cmp, gscore, max_gh);				

				cmp = _mm512_cmpgt_epi8_mask(j512, tail512); 
				max_gh = _mm512_mask_blend_epi8(cmp, max_gh, gscore);
				max_ie512 = _mm512_mask_blend_epi8(cmp, tmp512_1, max_ie512);
				gscore = max_gh;
				//_mm512_store_si512((__m512i *)(temp), gscore);
				//_mm512_store_si512((__m512i *)(temp1), exit0);
				//_mm512_store_si512((__m512i *)(temp2), maxRS1);
				//_mm512_store_si512((__m512i *)(temp3), tail512);
				//printf("i: %d, j: %d, minq: %d, gscore: %d, exit: %d, maxRS: %d, tail: %d\n",
				//	   i, j, minq, temp[1], temp1[1], temp2[1], temp3[1]);
			}
        }
		_mm512_store_si512((__m512i *)(H_h + j * SIMD_WIDTH8), h10);
		_mm512_store_si512((__m512i *)(F + j * SIMD_WIDTH8), zero512);
				
		
		/* exit due to zero score by a row */
		//__mmask64 cval = 0xFFFFFFFFFFFFFFFF;
		__mmask64 cval = dmask;
		__m512i bmaxScore512 = maxScore512;
		__mmask64 tmp = _mm512_cmpeq_epi8_mask(maxRS1, zero512);
		if (cval == tmp) break;

		exit0 = _mm512_mask_blend_epi8(tmp, exit0, zero512);
		//_mm512_store_si512((__m512i *)(temp1), exit0);
		
		__m512i score512 = _mm512_max_epi8(maxScore512, maxRS1);
		__mmask64 mex0 = _mm512_movepi8_mask(exit0);
		maxScore512 = _mm512_mask_blend_epi8(mex0, maxScore512, score512);

		__mmask64 cmp = _mm512_cmpgt_epi8_mask(maxScore512, bmaxScore512);
		y512 = _mm512_mask_blend_epi8(cmp, y512, y1_512);
		x512 = _mm512_mask_blend_epi8(cmp, x512, i1_512);
		
		/* max_off calculations */
		__m512i ind512 = _mm512_sub_epi8(y1_512, i1_512);
		ind512 = _mm512_abs_epi8(ind512);
		__m512i bmax_off512 = max_off512;
		ind512 = _mm512_max_epi8(max_off512, ind512);
		max_off512 = _mm512_mask_blend_epi8(cmp, bmax_off512, ind512);

		/* Z-score condition for exit */
		ZSCORE8(i1_512, y1_512);		
		
#if RDT
		prof[DP1][0] += _rdtsc() - tim1;
#endif
		
        /* Narrowing of the band */
		/* Part 1: From beg */
		// cval = 0xFFFFFFFFFFFFFFFF;
		cval = dmask;
		int l;		
 		for (l = beg; l < end; l++) {
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH8));
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH8));
			__m512i tmp = _mm512_or_si512(f512, h512);
			__mmask64 val = _mm512_cmpeq_epi8_mask(tmp, zero512);
			if (cval == val) nbeg = l;
			else
				break;
		}
		
		/* From end */
		bool flg = 1;
		for (l = end; l >= beg; l--) {
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH8));
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH8));
			__m512i tmp = _mm512_or_si512(f512, h512);
			__mmask64 val = _mm512_cmpeq_epi8_mask(tmp, zero512);
			if (val != cval && flg)  
				break;
		}
		nend = l + 2 < ncol? l + 2: ncol;

#if RDT
		tim1 = _rdtsc();
#endif
		/* Setting of head and tail for each pair */
		__m512i tail512_ = _mm512_sub_epi8(tail512, one512);
		//__m512i tail512_ = _mm512_sub_epi8(tail512, zero512);
		__m512i exit1 = _mm512_xor_si512(exit0, ff512);
		// __mmask64 tmpb = 0xFFFFFFFFFFFFFFFF;
		__mmask64 tmpb = dmask;
		__m512i l512 = _mm512_set1_epi8(beg);
		
		for (l = beg; l < end; l++) {
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH8));
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH8));	
			__m512i tmp_ = _mm512_or_si512(f512, h512);
			tmp_ = _mm512_or_si512(tmp_, exit1);			
			__mmask64 tmp = _mm512_cmpeq_epi8_mask(tmp_, zero512);
			if (tmp == 0x00) {
				break;
			}
			
			tmp = tmp & tmpb;
			l512 = _mm512_add_epi8(l512, one512);
#if !NEW
			__mmask64 mask2 = _mm512_cmpgt_epi8_mask(l512, tail512_);
			__m512i msk2 = _mm512_mask_blend_epi8(mask2, l512, tail512);
			head512 = _mm512_mask_blend_epi8(tmp, head512, msk2);
#else
			head512 = _mm512_mask_blend_epi8(tmp, head512, l512);
#endif
			tmpb = tmp;			
		}
		
		__m512i  index512 = tail512;
		// tmpb = 0xFFFFFFFFFFFFFFFF;
		tmpb = dmask;
		l512 = _mm512_set1_epi8(end);
		
		for (l = end; l >= beg; l--)
		{
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH8));
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH8));			
			__m512i tmp_ = _mm512_or_si512(f512, h512);
			tmp_ = _mm512_or_si512(tmp_, exit1);
			__mmask64 tmp = _mm512_cmpeq_epi8_mask(tmp_, zero512);			
			if (tmp == 0x00)  {
				break;
			}

			tmp = tmp & tmpb;
			l512 = _mm512_sub_epi8(l512, one512);
#if !NEW
			__mmask64 mask2 = _mm512_cmpgt_epi8_mask(l512, tail512);			
			__m512i msk2 = _mm512_mask_blend_epi8(mask2, l512, tail512);
			index512 = _mm512_mask_blend_epi8(tmp, index512, msk2);
#else
			index512 = _mm512_mask_blend_epi8(tmp, index512, l512);
#endif
			//l512 = _mm512_sub_epi8(l512, one512);
			tmpb = tmp;
		}
		index512 = _mm512_add_epi8(index512, two512);
		tail512 = _mm512_min_epi8(index512, qlen512);

#if RDT
		prof[DP2][0] += _rdtsc() - tim1;
#endif
    }
	
#if RDT
	prof[DP][0] += _rdtsc() - tim;
#endif
	
    int8_t score[SIMD_WIDTH8]  __attribute((aligned(64)));
	_mm512_store_si512((__m512i *) score, maxScore512);

	int8_t maxi[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) maxi, x512);

	int8_t maxj[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) maxj, y512);

	int8_t max_off_ar[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) max_off_ar, max_off512);

	int8_t gscore_ar[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) gscore_ar, gscore);

	int8_t maxie_ar[SIMD_WIDTH8]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) maxie_ar, max_ie512);
	
	static int cnt = 0;	
    for(i = 0; i < SIMD_WIDTH8; i++)
    {
		p[i].score = score[i];
		p[i].tle = maxi[i];
		p[i].qle = maxj[i];
		p[i].max_off = max_off_ar[i];
		p[i].gscore = gscore_ar[i];
		p[i].gtle = maxie_ar[i];
    }
	
    return;
}
//----------------------------AVX512 vec 16 bit SIMD lane -------------------------------------
#define PFD16 2
void BandedPairWiseSW::getScores16(SeqPair *pairArray,
								   uint8_t *seqBufRef,
								   uint8_t *seqBufQer,
								   int32_t numPairs,
								   uint16_t numThreads,
								   int8_t w)
{
    int i;
    int64_t startTick, endTick;
	// F16_ = H16_ = H16__ = NULL;
    // F16_ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);
    // H16_ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);
	// H16__ = (int16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(int16_t), 64);
	// if (F16_ == NULL || H16_ == NULL || H16__ == NULL) {
	// 	printf("Memory not alloacted!!!\n");
	// 	exit(0);
	// }   

	smithWatermanBatchWrapper16(pairArray, seqBufRef, seqBufQer, numPairs, numThreads, w);

	// _mm_free(F16_);_mm_free(H16_); _mm_free(H16__);
	
#if MAXI
	printf("AVX512 Vecor code: Writing output..\n");
	for (int l=0; l<numPairs; l++)
	{
		fprintf(stderr, "%d (%d %d) %d %d %d\n",
				pairArray[l].score, pairArray[l].tle, pairArray[l].qle,
				pairArray[l].gscore, pairArray[l].max_off, pairArray[l].gtle);

	}
	printf("Vector code: Writing output completed!!!\n\n");
#endif

}

void BandedPairWiseSW::smithWatermanBatchWrapper16(SeqPair *pairArray,
												   uint8_t *seqBufRef,
												   uint8_t *seqBufQer,
												   int32_t numPairs,
												   uint16_t numThreads,
												   int8_t w)
{
	// printf("numThreads: %d\n", numThreads);		
	int64_t st1, st2, st3, st4, st5;
    st1 = __rdtsc();
    uint16_t *seq1SoA = (uint16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(uint16_t), 64);
    uint16_t *seq2SoA = (uint16_t *)_mm_malloc(MAX_SEQ_LEN16 * SIMD_WIDTH16 * numThreads * sizeof(uint16_t), 64);

    int32_t ii;
    int32_t roundNumPairs = ((numPairs + SIMD_WIDTH16 - 1)/SIMD_WIDTH16 ) * SIMD_WIDTH16;
    for(ii = numPairs; ii < roundNumPairs; ii++)
    {
        pairArray[ii].id = ii;
        pairArray[ii].len1 = 0;
        pairArray[ii].len2 = 0;
    }

    st2 = __rdtsc();	
#if SORT_PAIRS
    // Sort the sequences according to decreasing order of lengths
    SeqPair *tempArray = (SeqPair *)_mm_malloc(SORT_BLOCK_SIZE * numThreads *
											   sizeof(SeqPair), 64);
    int16_t *hist = (int16_t *)_mm_malloc((MAX_SEQ_LEN16 + 32) * numThreads *
										  sizeof(int16_t), 64);
#pragma omp parallel num_threads(numThreads)
    {
        int32_t tid = omp_get_thread_num();
        SeqPair *myTempArray = tempArray + tid * SORT_BLOCK_SIZE;
        int16_t *myHist = hist + tid * (MAX_SEQ_LEN16 + 32);

#pragma omp for
        for(ii = 0; ii < roundNumPairs; ii+=SORT_BLOCK_SIZE)
        {
            int32_t first, last;
            first = ii;
            last  = ii + SORT_BLOCK_SIZE;
            if(last > roundNumPairs) last = roundNumPairs;
            sortPairsLen(pairArray + first, last - first, myTempArray, myHist);
        }
    }
    _mm_free(hist);
#endif
    st3 = __rdtsc();

#ifdef VTUNE_ANALYSIS
    __itt_resume();
#endif

	int eb = end_bonus;
#pragma omp parallel num_threads(numThreads)
    {
        int64_t st = __rdtsc();
        int32_t i;
        uint16_t tid = omp_get_thread_num();
        uint16_t *mySeq1SoA = seq1SoA + tid * MAX_SEQ_LEN16 * SIMD_WIDTH16;
        uint16_t *mySeq2SoA = seq2SoA + tid * MAX_SEQ_LEN16 * SIMD_WIDTH16;
        uint8_t *seq1;
        uint8_t *seq2;
		uint16_t h0[SIMD_WIDTH16];
		uint16_t band[SIMD_WIDTH16];		
		uint16_t qlen[SIMD_WIDTH16] __attribute__((aligned(64)));
		int16_t bsize = 0;
		__mmask16 dmask4 = 0xFFFF;
		
		int16_t *H1 = H16_ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;
		int16_t *H2 = H16__ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;

		__m512i zero512	  = _mm512_setzero_si512();
		__m512i o_ins512  = _mm512_set1_epi16(o_ins);
		__m512i e_ins512  = _mm512_set1_epi16(e_ins);
		__m512i oe_ins512 = _mm512_set1_epi16(o_ins + e_ins);
		__m512i o_del512  = _mm512_set1_epi16(o_del);
		__m512i e_del512  = _mm512_set1_epi16(e_del);
		__m512i eb_ins512 = _mm512_set1_epi16(eb - o_ins);
		__m512i eb_del512 = _mm512_set1_epi16(eb - o_del);
		
		int16_t max = 0;
		if (max < w_match) max = w_match;
		if (max < w_mismatch) max = w_mismatch;
		if (max < w_ambig) max = w_ambig;
		
		int nstart = 0, nend = numPairs;

#if DEB
		int div = (spot-1) / SIMD_WIDTH16;
		nstart = div * SIMD_WIDTH16;
		nend = (div+1) * SIMD_WIDTH16;
		int lane = (spot - 1) % SIMD_WIDTH16;
		printf("nstart: %d, nend: %d\n", nstart, nend);
#endif
		
#pragma omp for schedule(dynamic, 128)
		for(i = nstart; i < nend; i+=SIMD_WIDTH16)
		{
			// prof[4][0]++;
            int32_t j, k;
            uint16_t maxLen1 = 0;
            uint16_t maxLen2 = 0;
            uint16_t minLen1 = MAX_SEQ_LEN16 + 1;
            uint16_t minLen2 = MAX_SEQ_LEN16 + 1;
			//bsize = 100;
			bsize = w;
			uint64_t tim;
			// tim = _rdtsc();
            for(j = 0; j < SIMD_WIDTH16; j++)
            {
				{ // prefetch block
					SeqPair spf = pairArray[i + j + PFD16];
					//_mm_prefetch((const char*) seqBufRef + (int64_t)spf.id * MAX_SEQ_LEN_REF, 0);
					//_mm_prefetch((const char*) seqBufRef + (int64_t)spf.id * MAX_SEQ_LEN_REF + 64, 0);
					_mm_prefetch((const char*) seqBufRef + (int64_t)spf.idr, 0);
					_mm_prefetch((const char*) seqBufRef + (int64_t)spf.idr + 64, 0);					
				}
                SeqPair sp = pairArray[i + j];
				h0[j] = sp.h0;
                // seq1 = seqBufRef + (int64_t)sp.id * MAX_SEQ_LEN_REF;
				seq1 = seqBufRef + (int64_t)sp.idr;

                for(k = 0; k < sp.len1; k++)
                {
                    mySeq1SoA[k * SIMD_WIDTH16 + j] = (seq1[k] == AMBIG ? dmask4:seq1[k]);
					H2[k * SIMD_WIDTH16 + j] = 0;
                }
				
				qlen[j] = sp.len2 * max;
                if(maxLen1 < sp.len1) maxLen1 = sp.len1;
            }
			// prof[1][tid] += _rdtsc() - tim;

            for(j = 0; j < SIMD_WIDTH16; j++)
            {
                SeqPair sp = pairArray[i + j];
                for(k = sp.len1; k <= maxLen1; k++)
                {
                    mySeq1SoA[k * SIMD_WIDTH16 + j] = DUMMY1;
					H2[k * SIMD_WIDTH16 + j] = DUMMY1;
                }
            }
//--------------------
			__m512i h0_512 = _mm512_load_si512((__m512i*) h0);
			_mm512_store_si512((__m512i *) H2, h0_512);
			__m512i tmp512 = _mm512_sub_epi16(h0_512, o_del512);
			
			for(k = 1; k < maxLen1; k++) {
				tmp512 = _mm512_sub_epi16(tmp512, e_del512);
				__m512i tmp512_ = _mm512_max_epi16(tmp512, zero512);
				_mm512_store_si512((__m512i *)(H2 + k* SIMD_WIDTH16), tmp512_);
			}
//-------------------
			// tim = _rdtsc();
            for(j = 0; j < SIMD_WIDTH16; j++)
            {
				{ // prefetch block
					SeqPair spf = pairArray[i + j + PFD16];
					//_mm_prefetch((const char*) seqBufQer + (int64_t)spf.id * MAX_SEQ_LEN_QER, 0);
					//_mm_prefetch((const char*) seqBufQer + (int64_t)spf.id * MAX_SEQ_LEN_QER + 64, 0);
					_mm_prefetch((const char*) seqBufQer + (int64_t)spf.idq, 0);
					_mm_prefetch((const char*) seqBufQer + (int64_t)spf.idq + 64, 0);					
				}
				
                SeqPair sp = pairArray[i + j];
                // seq2 = seqBufQer + (int64_t)sp.id * MAX_SEQ_LEN_QER;
				seq2 = seqBufQer + (int64_t)sp.idq;
                for(k = 0; k < sp.len2; k++)
                {
                    mySeq2SoA[k * SIMD_WIDTH16 + j] = (seq2[k]==AMBIG?dmask4:seq2[k]);
					H1[k * SIMD_WIDTH16 + j] = 0;					
                }
                if(maxLen2 < sp.len2) maxLen2 = sp.len2;
            }
			// prof[1][tid] += _rdtsc() - tim;
			
            for(j = 0; j < SIMD_WIDTH16; j++)
            {
                SeqPair sp = pairArray[i + j];
                for(k = sp.len2; k <= maxLen2; k++)
                {
                    mySeq2SoA[k * SIMD_WIDTH16 + j] = DUMMY2;
					H1[k * SIMD_WIDTH16 + j] = 0;
                }
            }
//------------------------
			_mm512_store_si512((__m512i *) H1, h0_512);
			__mmask32 mask512 = _mm512_cmpgt_epi16_mask(h0_512, oe_ins512);
			tmp512 = _mm512_sub_epi16(h0_512, oe_ins512);
			tmp512 = _mm512_mask_blend_epi16(mask512, zero512, tmp512);
			_mm512_store_si512((__m512i *) (H1 + SIMD_WIDTH16), tmp512);

			for(k = 2; k < maxLen2; k++)
			{
				__m512i h1_512 = tmp512;
				tmp512 = _mm512_sub_epi16(h1_512, e_ins512);
				tmp512 = _mm512_max_epi16(tmp512, zero512);
				_mm512_store_si512((__m512i *)(H1 + k*SIMD_WIDTH16), tmp512);
            }			
//------------------------

			/* Banding calculation in pre-processing */
#if 1
			uint16_t myband[SIMD_WIDTH16] __attribute__((aligned(64)));
			uint16_t temp[SIMD_WIDTH16] __attribute__((aligned(64)));
			{
				__m512i qlen512 = _mm512_load_si512((__m512i *) qlen);
				__m512i sum512 = _mm512_add_epi16(qlen512, eb_ins512);
				_mm512_store_si512((__m512i *) temp, sum512);				
				for (int l=0; l<SIMD_WIDTH16; l++) {
					double val = temp[l]/e_ins + 1.0;
					int max_ins = val;
					max_ins = max_ins > 1? max_ins : 1;
					myband[l] = min(bsize, max_ins);
				}
				sum512 = _mm512_add_epi16(qlen512, eb_del512);
				_mm512_store_si512((__m512i *) temp, sum512);				
				for (int l=0; l<SIMD_WIDTH16; l++) {
					double val = temp[l]/e_del + 1.0;
					int max_ins = val;
					max_ins = max_ins > 1? max_ins : 1;
					myband[l] = min(myband[l], max_ins);
					bsize = bsize < myband[l] ? myband[l] : bsize;
				}				
			}
#endif

			// uint64_t tim_swa = _rdtsc();
            smithWaterman512_16(mySeq1SoA,
								mySeq2SoA,
								maxLen1,
								maxLen2,
								pairArray + i,
								h0,
								tid,
								numPairs,
								zdrop,
								bsize,
								qlen,
								myband);
			// prof[0][tid] += _rdtsc() - tim_swa;
        }
    }

#ifdef VTUNE_ANALYSIS
    __itt_pause();
#endif
	
    st4 = __rdtsc();
#if SORT_PAIRS
	{
    // Sort the sequences according to increasing order of id
#pragma omp parallel num_threads(numThreads)
    {
        int32_t tid = omp_get_thread_num();
        SeqPair *myTempArray = tempArray + tid * SORT_BLOCK_SIZE;

#pragma omp for
        for(ii = 0; ii < roundNumPairs; ii+=SORT_BLOCK_SIZE)
        {
            int32_t first, last;
            first = ii;
            last  = ii + SORT_BLOCK_SIZE;
            if(last > roundNumPairs) last = roundNumPairs;
            sortPairsId(pairArray + first, first, last - first, myTempArray);
        }
    }
    _mm_free(tempArray);
	}
#endif
	
    st5 = __rdtsc();
    setupTicks += st2 - st1;
    sort1Ticks += st3 - st2;
    swTicks += st4 - st3;
    sort2Ticks += st5 - st4;

	// free mem
	_mm_free(seq1SoA);
	_mm_free(seq2SoA);
	
    return;
}

void BandedPairWiseSW::smithWaterman512_16(uint16_t seq1SoA[],
										   uint16_t seq2SoA[],
										   uint16_t nrow,
										   uint16_t ncol,
										   SeqPair *p,
										   uint16_t *h0,
										   uint16_t tid,
										   int32_t numPairs,
										   int zdrop,
										   uint16_t w,
										   uint16_t qlen[],
										   uint16_t myband[])
{
#if DEB
	int16_t Hmat[nrow + 10][ncol + 10], Fmat[nrow + 10][ncol + 10];;
#endif
	
	static int cntr = 0;

    __m512i match512	 = _mm512_set1_epi16(this->w_match);
    __m512i mismatch512	 = _mm512_set1_epi16(this->w_mismatch);
    __m512i gapOpen512	 = _mm512_set1_epi16(this->w_open);
    __m512i gapExtend512 = _mm512_set1_epi16(this->w_extend);
	__m512i gapOE512	 = _mm512_set1_epi16(this->w_open + this->w_extend);
	__m512i w_ambig_512	 = _mm512_set1_epi16(this->w_ambig);	// ambig penalty
	__m512i five512		 = _mm512_set1_epi16(5);
	
	__m512i e_del512	= _mm512_set1_epi16(this->e_del);
	__m512i oe_del512	= _mm512_set1_epi16(this->o_del + this->e_del);
	__m512i e_ins512	= _mm512_set1_epi16(this->e_ins);
	__m512i oe_ins512	= _mm512_set1_epi16(this->o_ins + this->e_ins);

	int16_t	*F	 = F16_ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;
    int16_t	*H_h = H16_ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;
	int16_t	*H_v = H16__ + tid * SIMD_WIDTH16 * MAX_SEQ_LEN16;
	
	int lane = 0;
#if DEB
	lane = (spot - 1)%32;
#endif
	
    int16_t i, j;

	uint16_t tlen[SIMD_WIDTH16];
	uint16_t tail[SIMD_WIDTH16] __attribute((aligned(64)));
	uint16_t head[SIMD_WIDTH16] __attribute((aligned(64)));
	
	int minq = 1000;
	for (int l=0; l<SIMD_WIDTH16; l++) {
		tlen[l] = p[l].len1;
		qlen[l] = p[l].len2;
		if (p[l].len2 < minq) minq = p[l].len2;
	}
	minq -= 1; // for gscore

	__m512i tlen512	  = _mm512_load_si512((__m512i *) tlen);
	__m512i qlen512	  = _mm512_load_si512((__m512i *) qlen);
	__m512i myband512 = _mm512_load_si512((__m512i *) myband);
    __m512i zero512	  = _mm512_setzero_si512();
	__m512i one512	  = _mm512_set1_epi16(1);
	__m512i two512	  = _mm512_set1_epi16(2);
	__m512i i512_1	  = _mm512_set1_epi16(1);
	__m512i max_ie512 = zero512;
	__mmask16 dmask4 = 0xFFFF;
	__m512i ff512	  = _mm512_set1_epi16(dmask4);
	
   	__m512i tail512 = qlen512, head512 = zero512;
	_mm512_store_si512((__m512i *) head, head512);
   	_mm512_store_si512((__m512i *) tail, tail512);
	//__m512i ib512 = _mm512_add_epi16(qlen512, qlen512);
	// ib512 = _mm512_sub_epi16(ib512, one512);

	__m512i mlen512 = _mm512_add_epi16(qlen512, myband512);
	mlen512 = _mm512_min_epu16(mlen512, tlen512);
	
	uint16_t temp[SIMD_WIDTH16]  __attribute((aligned(64)));
	uint16_t temp1[SIMD_WIDTH16]  __attribute((aligned(64)));
	
	__m512i s00	 = _mm512_load_si512((__m512i *)(seq1SoA));
	__m512i hval = _mm512_load_si512((__m512i *)(H_v));
	__mmask32 dmask = 0xFFFFFFFF;
	
///////
	__m512i maxScore512 = hval;
    for(j = 0; j < ncol; j++)
		_mm512_store_si512((__m512i *)(F + j * SIMD_WIDTH16), zero512);
	
	__m512i x512	   = zero512;
	__m512i y512	   = zero512;
	__m512i i512	   = zero512;
	__m512i gscore	   = _mm512_set1_epi16(-1);
	__m512i max_off512 = zero512;
	__m512i exit0	   = _mm512_set1_epi16(dmask4);
	__m512i zdrop512   = _mm512_set1_epi16(zdrop);

	int beg = 0, end = ncol;
	int nbeg = beg, nend = end;

#if RDT
	uint64_t tim = _rdtsc();
#endif

    for(i = 0; i < nrow; i++)
    {
        __m512i e11 = zero512;
        __m512i h00, h11, h10;
        __m512i s10 = _mm512_load_si512((__m512i *)(seq1SoA + (i + 0) * SIMD_WIDTH16));

		beg = nbeg; end = nend;
		int pbeg = beg;
		if (beg < i - w) beg = i - w;
		if (end > i + w + 1) end = i + w + 1;
		if (end > ncol) end = ncol;

		h10 = zero512;
		if (beg == 0)
			h10 = _mm512_load_si512((__m512i *)(H_v + (i+1) * SIMD_WIDTH16));

		__m512i j512 = zero512;
		__m512i maxRS1, maxRS2, maxRS3, maxRS4;
		maxRS1 = zero512;
		
		__m512i i1_512 = _mm512_set1_epi16(i+1);
		__m512i y1_512 = zero512;
		
#if RDT	
		uint64_t tim1 = _rdtsc();
#endif
		
		/* Banding */
		__m512i i512, cache512, max512;
		__m512i phead512 = head512, ptail512 = tail512;
		i512 = _mm512_set1_epi16(i);
		cache512 = _mm512_sub_epi16(i512, myband512);
		head512	 = _mm512_max_epi16(head512, cache512);
		cache512 = _mm512_add_epi16(i1_512, myband512);
		tail512	 = _mm512_min_epu16(tail512, cache512);
		tail512	 = _mm512_min_epu16(tail512, qlen512);
     	/* Banding ends */
		
		// NEW, trimming.
		__mmask32 cmph = _mm512_cmpeq_epi16_mask(head512, phead512);
		__mmask32 cmpt = _mm512_cmpeq_epi16_mask(tail512, ptail512);
		cmph &= cmpt;
		for (int l=beg; l<end && cmph != dmask; l++) {
			// for (int l=beg; l<end; l++) {
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH16));
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH16));

			__m512i pj512 = _mm512_set1_epi16(l);
			__m512i j512 = _mm512_set1_epi16(l+1);
			__mmask32 cmp1 = _mm512_cmpgt_epi16_mask(head512, pj512);
			if (cmp1 == 0x00) break;
			// __mmask32 cmp2 = _mm512_cmpgt_epi16_mask(pj512, tail512);
			__mmask32 cmp2 = _mm512_cmpgt_epi16_mask(j512, tail512);
			cmp1 = cmp1 | cmp2;
			h512 = _mm512_mask_blend_epi16(cmp1, h512, zero512);
			f512 = _mm512_mask_blend_epi16(cmp1, f512, zero512);

			_mm512_store_si512((__m512i *)(F + l * SIMD_WIDTH16), f512);
			_mm512_store_si512((__m512i *)(H_h + l * SIMD_WIDTH16), h512);
		}

#if RDT
		prof[DP3][0] += _rdtsc() - tim1;
#endif

#if DEB
		_mm512_store_si512((__m512i *) head, head512);
		_mm512_store_si512((__m512i *) tail, tail512);
		
		for (int l=0; l<=ncol; l++)
			Hmat[i][l] = H_h[l * SIMD_WIDTH16 + lane];
		
		_mm512_store_si512((__m512i *) temp, exit0);
		if (temp[lane]) {
			printf("%d, h: %d, t: %d, myband: %d, beg: %d, end: %d, exit: %d\n",
				   i, head[lane], tail[lane], myband[lane], beg, end, temp[lane]);
		}		
#endif
		// beg = nbeg; end = nend;
		__mmask32 cmp512_1 = _mm512_cmpgt_epi16_mask(i1_512, tlen512);

		/* Updating row exit status */
		__mmask32 cmpim = _mm512_cmpgt_epi16_mask(i1_512, mlen512);
		__mmask32 cmpht = _mm512_cmpeq_epi16_mask(tail512, head512);
		cmpim = cmpim | cmpht;
#if NEW
		cmpht = _mm512_cmpgt_epi16_mask(head512, tail512);
		cmpim = cmpim |  cmpht;
#endif
		exit0 = _mm512_mask_blend_epi16(cmpim, exit0, zero512);

		
#if RDT
		tim1 = _rdtsc();
#endif
		
		j512 = _mm512_set1_epi16(beg);
		for(j = beg; j < end; j++)
		{
            __m512i f11, f21, f31, f41, f51, jj512, s2;
			h00 = _mm512_load_si512((__m512i *)(H_h + j * SIMD_WIDTH16));
            f11 = _mm512_load_si512((__m512i *)(F + j * SIMD_WIDTH16));

            s2 = _mm512_load_si512((__m512i *)(seq2SoA + (j) * SIMD_WIDTH16));
			
			__m512i pj512 = j512;
			j512 = _mm512_add_epi16(j512, one512);

            //MAIN_CODE16(s10, s2, h00, h11, e11, f11, f21, zero512,
			//		  maxScore512, gapOpen512, gapExtend512, y1_512, maxRS1); //i+1
			MAIN_CODE16(s10, s2, h00, h11, e11, f11, f21, zero512,
					   maxScore512, e_ins512, oe_ins512,
					   e_del512, oe_del512, gapExtend512,
					   y1_512, maxRS1); //i+1

			// Masked writing
			__mmask32 cmp2 = _mm512_cmpgt_epi16_mask(head512, pj512);
			__mmask32 cmp1 = _mm512_cmpgt_epi16_mask(pj512, tail512);
			cmp1 = cmp1 | cmp2;
			h10 = _mm512_mask_blend_epi16(cmp1, h10, zero512);
			f21 = _mm512_mask_blend_epi16(cmp1, f21, zero512);
			
			/* Part of main code MAIN_CODE */
			__m512i bmaxRS = maxRS1, blend512;										
			maxRS1 =_mm512_max_epi16(maxRS1, h11);							
			__mmask32 cmpA = _mm512_cmpgt_epi16_mask(maxRS1, bmaxRS);					
			__mmask32 cmpB =_mm512_cmpeq_epi16_mask(maxRS1, h11);					
			cmpA = cmpA | cmpB;
			cmp1 = _mm512_cmpgt_epi16_mask(j512, tail512);
			cmp1 = cmp1 | cmp2;			
			blend512 = _mm512_mask_blend_epi16(cmpA, y1_512, j512);
			y1_512 = _mm512_mask_blend_epi16(cmp1, blend512, y1_512);
			maxRS1 = _mm512_mask_blend_epi16(cmp1, maxRS1, bmaxRS);						

			_mm512_store_si512((__m512i *)(F + j * SIMD_WIDTH16), f21);
			_mm512_store_si512((__m512i *)(H_h + j * SIMD_WIDTH16), h10);

			h10 = h11;
			
			
			/* gscore calculations */
			if (j >= minq)
			{
				__mmask32 cmp = _mm512_cmpeq_epi16_mask(j512, qlen512);
				__m512i max_gh = _mm512_max_epi16(gscore, h11);
				__mmask32 cmp_gh = _mm512_cmpgt_epi16_mask(gscore, h11);
				__m512i tmp512_1 = _mm512_mask_blend_epi16(cmp_gh, i1_512, max_ie512);

				tmp512_1 = _mm512_mask_blend_epi16(cmp, max_ie512, tmp512_1);
				__mmask32 mex0 = _mm512_movepi16_mask(exit0);
				tmp512_1 = _mm512_mask_blend_epi16(mex0, max_ie512, tmp512_1);
				
				max_gh = _mm512_mask_blend_epi16(mex0, gscore, max_gh);
				max_gh = _mm512_mask_blend_epi16(cmp, gscore, max_gh);				

				cmp = _mm512_cmpgt_epi16_mask(j512, tail512); 
				max_gh = _mm512_mask_blend_epi16(cmp, max_gh, gscore);
				max_ie512 = _mm512_mask_blend_epi16(cmp, tmp512_1, max_ie512);
				gscore = max_gh;
			}
        }
		_mm512_store_si512((__m512i *)(H_h + j * SIMD_WIDTH16), h10);
		_mm512_store_si512((__m512i *)(F + j * SIMD_WIDTH16), zero512);
				
		
		/* exit due to zero score by a row */
		__mmask32 cval = dmask;
		__m512i bmaxScore512 = maxScore512;
		__mmask32 tmp = _mm512_cmpeq_epi16_mask(maxRS1, zero512);
		if (cval == tmp) break;

		exit0 = _mm512_mask_blend_epi16(tmp, exit0, zero512);

		__m512i score512 = _mm512_max_epi16(maxScore512, maxRS1);
		__mmask32 mex0 = _mm512_movepi16_mask(exit0);
		maxScore512 = _mm512_mask_blend_epi16(mex0, maxScore512, score512);

		__mmask32 cmp = _mm512_cmpgt_epi16_mask(maxScore512, bmaxScore512);
		y512 = _mm512_mask_blend_epi16(cmp, y512, y1_512);
		x512 = _mm512_mask_blend_epi16(cmp, x512, i1_512);
		
		/* max_off calculations */
		__m512i ind512 = _mm512_sub_epi16(y1_512, i1_512);
		ind512 = _mm512_abs_epi16(ind512);
		__m512i bmax_off512 = max_off512;
		ind512 = _mm512_max_epi16(max_off512, ind512);
		max_off512 = _mm512_mask_blend_epi16(cmp, bmax_off512, ind512);

		/* Z-score condition for exit */
		ZSCORE16(i1_512, y1_512);		

#if RDT
		prof[DP1][0] += _rdtsc() - tim1;
#endif
		
        /* Narrowing of the band */
		/* Part 1: From beg */
		cval = dmask;
		int l;		
 		for (l = beg; l < end; l++) {
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH16));
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH16));
			__m512i tmp = _mm512_or_si512(f512, h512);
			__mmask32 val = _mm512_cmpeq_epi16_mask(tmp, zero512);
			if (cval == val) nbeg = l;
			else
				break;
		}
		
		/* From end */
		bool flg = 1;
		for (l = end; l >= beg; l--) {
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH16));
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH16));
			__m512i tmp = _mm512_or_si512(f512, h512);
			__mmask32 val = _mm512_cmpeq_epi16_mask(tmp, zero512);
			if (val != cval && flg)  
				break;
		}
		nend = l + 2 < ncol? l + 2: ncol;

#if RDT
		tim1 = _rdtsc();
#endif
		/* Setting of head and tail for each pair */
		beg = nbeg; end = l; // keep check on this!!
		
		__m512i tail512_ = _mm512_sub_epi16(tail512, one512);
		__m512i exit1 = _mm512_xor_si512(exit0, ff512);
		__mmask32 tmpb = dmask;
		__m512i l512 = _mm512_set1_epi16(beg);
		
		for (l = beg; l < end; l++) {
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH16));
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH16));	
			__m512i tmp_ = _mm512_or_si512(f512, h512);
			tmp_ = _mm512_or_si512(tmp_, exit1);			
			__mmask32 tmp = _mm512_cmpeq_epi16_mask(tmp_, zero512);
			if (tmp == 0x00) {
				break;
			}
			
			tmp = tmp & tmpb;
			l512 = _mm512_add_epi16(l512, one512);
#if !NEW
			__mmask32 mask2 = _mm512_cmpgt_epi16_mask(l512, tail512_);
			__m512i msk2 = _mm512_mask_blend_epi16(mask2, l512, tail512);
			head512 = _mm512_mask_blend_epi16(tmp, head512, msk2);
#else
			head512 = _mm512_mask_blend_epi16(tmp, head512, l512);
#endif
			tmpb = tmp;			
		}
		
		__m512i  index512 = tail512;
		tmpb = dmask;
		l512 = _mm512_set1_epi16(end);
		
		for (l = end; l >= beg; l--)
		{
			__m512i f512 = _mm512_load_si512((__m512i *)(F + l * SIMD_WIDTH16));
			__m512i h512 = _mm512_load_si512((__m512i *)(H_h + l * SIMD_WIDTH16));			
			__m512i tmp_ = _mm512_or_si512(f512, h512);
			tmp_ = _mm512_or_si512(tmp_, exit1);
			__mmask32 tmp = _mm512_cmpeq_epi16_mask(tmp_, zero512);			
			if (tmp == 0x00)  {
				break;
			}

			tmp = tmp & tmpb;
			l512 = _mm512_sub_epi16(l512, one512);
#if !NEW
			__mmask32 mask2 = _mm512_cmpgt_epi16_mask(l512, tail512);			
			__m512i msk2 = _mm512_mask_blend_epi16(mask2, l512, tail512);
			index512 = _mm512_mask_blend_epi16(tmp, index512, msk2);
#else
			index512 = _mm512_mask_blend_epi16(tmp, index512, l512);
#endif
			tmpb = tmp;
		}
		index512 = _mm512_add_epi16(index512, two512);
		tail512 = _mm512_min_epi16(index512, qlen512);

#if RDT
		prof[DP2][0] += _rdtsc() - tim1;
#endif
    }
	
#if RDT
	prof[DP][0] += _rdtsc() - tim;
#endif
	
    int16_t score[SIMD_WIDTH16]  __attribute((aligned(64)));
	_mm512_store_si512((__m512i *) score, maxScore512);

	int16_t maxi[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) maxi, x512);

	int16_t maxj[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) maxj, y512);

	int16_t max_off_ar[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) max_off_ar, max_off512);

	int16_t gscore_ar[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) gscore_ar, gscore);

	int16_t maxie_ar[SIMD_WIDTH16]  __attribute((aligned(64)));
    _mm512_store_si512((__m512i *) maxie_ar, max_ie512);
	
	static int cnt = 0;	
    for(i = 0; i < SIMD_WIDTH16; i++)
    {
		p[i].score = score[i];
		p[i].tle = maxi[i];
		p[i].qle = maxj[i];
		p[i].max_off = max_off_ar[i];
		p[i].gscore = gscore_ar[i];
		p[i].gtle = maxie_ar[i];
    }	

    return;
}
#endif