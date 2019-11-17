#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include "utils.h"
#include "ertindex.h"

#ifdef USE_MALLOC_WRAPPERS
#  include "malloc_wrap.h"
#endif

#define _set_pac(pac, l, c) ((pac)[(l)>>2] |= (c)<<(((l)&3)<<1))
#define _set_pac_orig(pac, l, c) ((pac)[(l)>>2] |= (c)<<((~(l)&3)<<1))

inline void getNumBranchesForKmer(bwtintv_t ok[4], uint8_t* numBranches, uint8_t* uniform_bp) {
    uint8_t i;
    for (i = 0; i < 4; ++i) {
        if (ok[i].x[2] > 0) { *numBranches += 1; *uniform_bp = i; }
    }
}

static inline void kmertoquery(uint64_t x, uint8_t *a, int l) {
    int i;
    for (i = 0; i < l; ++i) {
        a[i] = (uint8_t)((x >> (i << 1)) & 0x3);
    }
}

inline uint64_t addBytesForEntry(byte_type_t type, int count, int numHits) {
    uint64_t numBits = 0;
    switch(type) {
        case CODE:
            numBits = 8;
            break;
        case LEAF_COUNT:
            numBits = (hits_count_size_in_bits); 
            break;
        case LEAF_HITS:
            numBits = (ref_ptr_size_in_bits * numHits); 
            break;
        case UNIFORM_COUNT: // "Uniform"
            numBits = (char_count_size_in_bits);
            break;
        case UNIFORM_BP:
            numBits = (count << 1);
            break;
        case LEAF_PTR: // "Leaf Offset Pointer"
            numBits = leaf_offset_ptr_size_in_bits;
            break;
        case OTHER_PTR:
            numBits = other_offset_ptr_size_in_bits;
            break;
        case EMPTY_NODE:
            numBits = 0;
            break;
        default :
            break;
    }
    return (numBits % 8) ? (numBits / 8 + 1) : numBits / 8;
}

void addChildNode(node_t* p, node_t* c) {
    assert(p->numChildren <= 3); 
    p->child_nodes[p->numChildren++] = c;
    c->parent_node = p;
}

void handleLeaf(const bwt_t* bwt, const bntseq_t *bns, const uint8_t *pac, bwtintv_t ik, node_t* n, int step) {
    n->type = LEAF;
    n->numHits = ik.x[2];
    n->hits = (uint64_t*) calloc(n->numHits, sizeof(uint64_t));
    n->leaf_prefix = (uint8_t**) calloc(n->numHits, sizeof(uint8_t*));
    if (step == 2) {
        uint64_t ref_pos = 0;
        int j = 0;
        for (j = 0; j < n->numHits; ++j) {
            ref_pos = bwt_sa(bwt, ik.x[0]+j);
            n->hits[j] = ref_pos;
            n->leaf_prefix[j] = (uint8_t*) calloc(PREFIX_LENGTH, sizeof(uint8_t));
            int64_t len;
            /// Fetch reference
            uint8_t* rseq = bns_get_seq(bns->l_pac, pac, ref_pos-PREFIX_LENGTH, ref_pos, &len);
            // FIXME: If len == 0, we need to decide on what prefix to store for strings spanning the forward-reverse boundary
            // assert(len != 0);
            if (len > 0) {
                /// Add prefix
                memcpy(n->leaf_prefix[j], rseq, len);
                free(rseq);
            }
        }
    }
}

void handleDivergence(const bwt_t* bwt, const bntseq_t *bns, const uint8_t *pac, 
                      bwtintv_t ok[4], int depth, node_t* parent_node, int step, int max_depth) {
    int i;
    bwtintv_t ok_copy[4];
    bwtintv_t ik_new;
    memcpy(ok_copy, ok, 4*sizeof(bwtintv_t));
    for (i = 3; i >= 0; --i) {
        node_t* n = (node_t*) calloc(1, sizeof(node_t));
        n->numChildren = 0;
        memset(n->child_nodes, 0, 4*sizeof(node_t*)); 
        n->pos = 0;
        n->num_bp = 0;
        if (ok_copy[i].x[2] == 0) { //!< Empty node
            n->type = EMPTY;
            n->numHits = 0;
            memcpy(n->seq, parent_node->seq, (depth-1)*sizeof(uint8_t));
            n->l_seq = depth;
            addChildNode(parent_node, n);
        }
        else if (ok_copy[i].x[2] > 1 && depth != max_depth) {
            ik_new = ok_copy[i]; ik_new.info = depth+1;
            memcpy(n->seq, parent_node->seq, parent_node->l_seq*sizeof(uint8_t));
            n->seq[depth] = i;
            n->pos = depth;
            n->num_bp = 1;
            n->l_seq = depth+1;
            n->numHits = ok_copy[i].x[2];
            n->type = DIVERGE;
            addChildNode(parent_node, n);
            ert_build_kmertree(bwt, bns, pac, ik_new, ok, depth+1, n, step, max_depth);
        }
        else {
            memcpy(n->seq, parent_node->seq, parent_node->l_seq*sizeof(uint8_t));
            n->seq[depth] = i;
            n->pos = depth;
            n->num_bp = 1;
            n->l_seq = depth+1;
            handleLeaf(bwt, bns, pac, ok_copy[i], n, step);
            addChildNode(parent_node, n);
        }
    }   
}

void ert_build_kmertree(const bwt_t* bwt, const bntseq_t *bns, const uint8_t *pac, 
                        bwtintv_t ik, bwtintv_t ok[4], int curDepth, node_t* parent_node, int step, int max_depth) {

    uint8_t uniform_bp = 0;
    uint8_t numBranches = 0, depth = curDepth;
    bwt_extend(bwt, &ik, ok, 0); //!< Extend right by 1bp
    /// Check if we need to make a uniform entry
    getNumBranchesForKmer(ok, &numBranches, &uniform_bp);
    bwtintv_t ik_new;
    /// If we find a uniform entry, extend further till we diverge or hit a leaf node
    if (numBranches == 1) {
        uint8_t uniformExtend = 1;
        ik_new = ok[uniform_bp]; ik_new.info = depth+1;
        node_t* n = (node_t*) calloc(1, sizeof(node_t));
        n->numChildren = 0;
        memset(n->child_nodes, 0, 4*sizeof(node_t*)); 
        memcpy(n->seq, parent_node->seq, parent_node->l_seq*sizeof(uint8_t));
        n->seq[depth] = uniform_bp;
        n->numHits = ok[uniform_bp].x[2];
        n->l_seq = depth + 1;
        n->pos = depth;
        n->num_bp = 1;
        addChildNode(parent_node, n);
        if (depth < max_depth) {
            while (uniformExtend) {
                numBranches = 0; uniform_bp = 0;
                depth += 1;
                bwt_extend(bwt, &ik_new, ok, 0); //!< Extend right by 1bp
                getNumBranchesForKmer(ok, &numBranches, &uniform_bp);
                assert(numBranches != 0);
                if (numBranches == 1) { //<! Uniform
                    ik_new = ok[uniform_bp]; ik_new.info = depth+1;
                    n->seq[depth] = uniform_bp;
                    n->l_seq = depth + 1;
                    n->num_bp++;
                    /// Hit a multi-hit leaf node
                    if (depth == max_depth) {
                        uniformExtend = 0;
                        handleLeaf(bwt, bns, pac, ok[uniform_bp], n, step);
                    }
                }
                else { //!< Diverge
                    uniformExtend = 0;
                    n->type = UNIFORM; 
                    handleDivergence(bwt, bns, pac, ok, depth, n, step, max_depth);                        
                }
            }
        }
        else { //<! Uniform, depth == max_depth, multi-hit leaf node
            uniformExtend = 0; 
            handleLeaf(bwt, bns, pac, ok[uniform_bp], n, step);
        }
    } //!< End uniform entry
    else { //!< Diverge, empty or leaf, same as above
        handleDivergence(bwt, bns, pac, ok, depth, parent_node, step, max_depth);                        
    } //!< End diverge
}

void ert_build_table_sl(const bwt_t* bwt, const bntseq_t *bns, const uint8_t *pac,
                        bwtintv_t ik, bwtintv_t ok[4], uint8_t* mlt_data, uint8_t* leaf_data,  
                        uint64_t* size, uint64_t* leaf_tbl_size, uint8_t* aq, 
                        uint64_t* numHits, uint64_t* max_next_ptr, uint64_t next_ptr_width,
                        int step, int max_depth, uint64_t* maxLeafPointer) {

    uint64_t byte_idx = *size;
    uint64_t leaf_byte_idx = *leaf_tbl_size;
    int i,j;
    uint8_t aq1[xmerSize];
    assert(xmerSize <= 15);
    uint64_t lep1 = 0;
    uint8_t c;
    uint64_t prevHits = ik.x[2];
    bwtintv_t ik_copy = ik;
    uint64_t mlt_byte_idx = byte_idx + (numXmers << 3);
    uint64_t xmer_entry = 0;
    uint16_t xmer_data = 0;
    uint64_t mlt_offset = mlt_byte_idx;
    for (i = 0; i < numXmers; ++i) {
        kmertoquery(i, aq1, xmerSize);
        for (j = 0; j < xmerSize; ++j) {
            c = 3 - aq1[j];
            bwt_extend(bwt, &ik, ok, 0); //!< ok contains the result of BWT extension
            if (ok[c].x[2] != prevHits) { //!< hit set changes
                lep1 |= (1 << j);
            }
            /// Extend right till k-mer has zero hits
            if (ok[c].x[2] >= 1) { prevHits = ok[c].x[2]; ik = ok[c]; ik.info = kmerSize + j + 1; }
            else { break; }
        }
        uint64_t num_hits = ok[c].x[2];
        if (ok[c].x[2] == 0) {
            xmer_data = ((lep1 & LEP_MASK) << METADATA_BITWIDTH) | INVALID;
        }
        else if (ok[c].x[2] == 1) {
            xmer_data = ((lep1 & LEP_MASK) << METADATA_BITWIDTH) | (SINGLE_HIT_LEAF);
            if (step == 2) {
                mlt_data[mlt_byte_idx] = 0; //!< Not a multi-hit
            }
            mlt_byte_idx++;
            if (step == 2) {
                uint64_t ref_pos = 0;
                ref_pos = bwt_sa(bwt, ok[c].x[0]);
                int64_t len;
                /// Fetch reference
                uint8_t* rseq = bns_get_seq(bns->l_pac, pac, ref_pos-PREFIX_LENGTH, ref_pos, &len);
                // FIXME: If len == 0, we need to decide on what prefix to store for strings spanning the forward-reverse boundary
                if (len > 0) {
                    /// Add prefix
                    uint8_t packed_prefix[1];                                        
                    memset(packed_prefix, 0, sizeof(uint8_t));                       
                    for (j = 0; j < PREFIX_LENGTH; ++j) {                                        
                        _set_pac(packed_prefix, j, rseq[j]);               
                    }
                    uint64_t leaf_data = (ref_pos << 7) | (packed_prefix[0] << 1);                    
                    memcpy(&mlt_data[mlt_byte_idx], &leaf_data, 5);                  
                    free(rseq);
                }
            }
            mlt_byte_idx += 5;
            *numHits += 1;
        }
        else {
            xmer_data = ((lep1 & LEP_MASK) << METADATA_BITWIDTH) | (INFREQUENT);
            node_t* n = (node_t*) calloc(1, sizeof(node_t));
            n->type = DIVERGE;
            n->pos = 0;
            n->num_bp = 0;
            memcpy(n->seq, aq, kmerSize);
            n->l_seq = kmerSize;
            memcpy(&n->seq[n->l_seq], aq1, xmerSize);
            n->l_seq += xmerSize;
            n->parent_node = 0;
            n->numChildren = 0;
            memset(n->child_nodes, 0, 4*sizeof(node_t*));
            n->start_addr = mlt_byte_idx;
            n->numHits = num_hits;
            ert_build_kmertree(bwt, bns, pac, ik, ok, kmerSize+j, n, step, max_depth);
            ert_traverse_kmertree_sl(n, mlt_data, leaf_data, &mlt_byte_idx, &leaf_byte_idx, kmerSize+j, numHits, 
                                     max_next_ptr, next_ptr_width, step, maxLeafPointer);
            ert_destroy_kmertree(n);
        }
        if (num_hits < 20) {
            xmer_entry = (mlt_offset << KMER_DATA_BITWIDTH) | (num_hits << 17) | xmer_data;
        }
        else {
            xmer_entry = (mlt_offset << KMER_DATA_BITWIDTH) | xmer_data;
        }
        uint64_t ptr_width = (next_ptr_width < 4) ? next_ptr_width : 0;
        xmer_entry |= (ptr_width << 22);
        if (step == 2) {
            memcpy(&mlt_data[byte_idx], &xmer_entry, 8);
        }
        byte_idx += 8;
        mlt_offset = mlt_byte_idx;
        ik = ik_copy;
        prevHits = ik_copy.x[2];
    }
    *size = mlt_byte_idx;
    *leaf_tbl_size = leaf_byte_idx;
}

void ert_build_table(const bwt_t* bwt, const bntseq_t *bns, const uint8_t *pac,
                     bwtintv_t ik, bwtintv_t ok[4], uint8_t* mlt_data, uint8_t* mh_data,  
                     uint64_t* size, uint64_t* mh_size, uint8_t* aq, 
                     uint64_t* numHits, uint64_t* max_next_ptr, uint64_t next_ptr_width,
                     int step, int max_depth) {

    uint64_t byte_idx = *size;
    uint64_t mh_byte_idx = *mh_size;
    int i,j;
    uint8_t aq1[xmerSize];
    assert(xmerSize <= 15);
    uint64_t lep1 = 0;
    uint8_t c;
    uint64_t prevHits = ik.x[2];
    bwtintv_t ik_copy = ik;
    uint64_t mlt_byte_idx = byte_idx + (numXmers << 3);
    uint64_t xmer_entry = 0;
    uint16_t xmer_data = 0;
    uint64_t mlt_offset = mlt_byte_idx;
    for (i = 0; i < numXmers; ++i) {
        kmertoquery(i, aq1, xmerSize);
        for (j = 0; j < xmerSize; ++j) {
            c = 3 - aq1[j];
            bwt_extend(bwt, &ik, ok, 0); //!< ok contains the result of BWT extension
            if (ok[c].x[2] != prevHits) { //!< hit set changes
                lep1 |= (1 << j);
            }
            /// Extend right till k-mer has zero hits
            if (ok[c].x[2] >= 1) { prevHits = ok[c].x[2]; ik = ok[c]; ik.info = kmerSize + j + 1; }
            else { break; }
        }
        uint64_t num_hits = ok[c].x[2];
        if (ok[c].x[2] == 0) {
            xmer_data = ((lep1 & LEP_MASK) << METADATA_BITWIDTH) | INVALID;
        }
        else if (ok[c].x[2] == 1) {
            xmer_data = ((lep1 & LEP_MASK) << METADATA_BITWIDTH) | (SINGLE_HIT_LEAF);
            if (step == 2) {
                mlt_data[mlt_byte_idx] = 0; //!< Not a multi-hit
            }
            mlt_byte_idx++;
            if (step == 2) {
                uint64_t ref_pos = 0;
                ref_pos = bwt_sa(bwt, ok[c].x[0]);
                int64_t len;
                /// Fetch reference
                uint8_t* rseq = bns_get_seq(bns->l_pac, pac, ref_pos-PREFIX_LENGTH, ref_pos, &len);
                // FIXME: If len == 0, we need to decide on what prefix to store for strings spanning the forward-reverse boundary
                if (len > 0) {
                    /// Add prefix
                    uint8_t packed_prefix[1];                                        
                    memset(packed_prefix, 0, sizeof(uint8_t));                       
                    for (j = 0; j < PREFIX_LENGTH; ++j) {                                        
                        _set_pac(packed_prefix, j, rseq[j]);               
                    }
                    uint64_t leaf_data = (ref_pos << 7) | (packed_prefix[0] << 1);                    
                    memcpy(&mlt_data[mlt_byte_idx], &leaf_data, 5);                  
                    free(rseq);
                }
            }
            mlt_byte_idx += 5;
            *numHits += 1;
        }
        else {
            xmer_data = ((lep1 & LEP_MASK) << METADATA_BITWIDTH) | (INFREQUENT);
            node_t* n = (node_t*) calloc(1, sizeof(node_t));
            n->type = DIVERGE;
            n->pos = 0;
            n->num_bp = 0;
            memcpy(n->seq, aq, kmerSize);
            n->l_seq = kmerSize;
            memcpy(&n->seq[n->l_seq], aq1, xmerSize);
            n->l_seq += xmerSize;
            n->parent_node = 0;
            n->numChildren = 0;
            memset(n->child_nodes, 0, 4*sizeof(node_t*));
            n->start_addr = mlt_byte_idx;
            ert_build_kmertree(bwt, bns, pac, ik, ok, kmerSize+j, n, step, max_depth);
            ert_traverse_kmertree(n, mlt_data, mh_data, &mlt_byte_idx, &mh_byte_idx, kmerSize+j, numHits, 
                                  max_next_ptr, next_ptr_width, step);
            ert_destroy_kmertree(n);
        }
        if (num_hits < 20) {
            xmer_entry = (mlt_offset << KMER_DATA_BITWIDTH) | (num_hits << 17) | xmer_data;
        }
        else {
            xmer_entry = (mlt_offset << KMER_DATA_BITWIDTH) | xmer_data;
        }
        uint64_t ptr_width = (next_ptr_width < 4) ? next_ptr_width : 0;
        xmer_entry |= (ptr_width << 22);
        if (step == 2) {
            memcpy(&mlt_data[byte_idx], &xmer_entry, 8);
        }
        byte_idx += 8;
        mlt_offset = mlt_byte_idx;
        ik = ik_copy;
        prevHits = ik_copy.x[2];
    }
    *size = mlt_byte_idx;
    *mh_size = mh_byte_idx;
}

void addCode(uint8_t* mlt_data, uint64_t* byte_idx, uint8_t code, int step) {
    if (step == 2) {
        memcpy(&mlt_data[*byte_idx], &code, sizeof(uint8_t));
    }
    *byte_idx += 1;
}

void addUniformNode(uint8_t* mlt_data, uint64_t* byte_idx, uint8_t count, uint8_t* uniformBases, uint64_t hitCount, int step) {
    uint8_t numBytesForBP = addBytesForEntry(UNIFORM_BP, count, 0);
    if (step == 2) {
        memcpy(&mlt_data[*byte_idx], &count, sizeof(uint8_t));
    }
    *byte_idx += 1;
    if (step == 2) {
        int j;
        uint8_t packUniformBases[numBytesForBP];
        memset(packUniformBases, 0, numBytesForBP);
        for (j = 0; j < count; ++j) {
            _set_pac_orig(packUniformBases, j, uniformBases[j]);
        }
        memcpy(&mlt_data[*byte_idx], packUniformBases, numBytesForBP); 
    }
    *byte_idx += numBytesForBP;
} 

void addLeafNode(uint8_t* mlt_data, uint64_t* byte_idx, uint64_t ref_pos, uint8_t** leaf_prefix, int step) {
    if (step == 2) {
        int j;
        uint8_t packed_prefix[1];
        memset(packed_prefix, 0, sizeof(uint8_t));
        for (j = 0; j < PREFIX_LENGTH; ++j) {
            _set_pac(packed_prefix, j, leaf_prefix[0][j]);
        }
        uint64_t leaf_data = (ref_pos << 7) | (packed_prefix[0] << 1);
        memcpy(&mlt_data[*byte_idx], &leaf_data, 5);
    }
    *byte_idx += 5;
}

void addMultiHitLeafNode(uint8_t* mlt_data, uint64_t* byte_idx, uint64_t count, uint64_t* hits, uint8_t** leaf_prefix, int step) {
    uint16_t k = 0;
    for (k = 0; k < count; ++k) {
        if (step == 2) {
            int j;
            uint8_t packed_prefix[1];
            memset(packed_prefix, 0, sizeof(uint8_t));
            for (j = 0; j < PREFIX_LENGTH; ++j) {
                _set_pac(packed_prefix, j, leaf_prefix[k][j]);
            }
            uint64_t leaf_data = (hits[k] << 7) | (packed_prefix[0] << 1) | 1ULL;
            memcpy(&mlt_data[*byte_idx], &leaf_data, 5);
        }
        *byte_idx += 5;
    }
} 

void addMultiHitLeafCount(uint8_t* mlt_data, uint64_t* byte_idx, uint64_t count, int step) {
    if (step == 2) {
        memcpy(&mlt_data[*byte_idx], &count, 2);
    }
    *byte_idx += 2;
}

void addMultiHitLeafPtr(uint8_t* mlt_data, uint64_t* byte_idx, uint64_t mh_byte_idx, int step) {
    if (step == 2) {
        uint64_t mh_data = (mh_byte_idx << 1) | 1ULL; 
        memcpy(&mlt_data[*byte_idx], &mh_data, 5);
    }
    *byte_idx += 5;
} 

void addMultiHitInfo(uint8_t* mlt_data, uint64_t* byte_idx, uint8_t mh, int step) {
    if (step == 2) {
        memcpy(&mlt_data[*byte_idx], &mh, sizeof(uint8_t));
    }
    *byte_idx += 1;
}

void setPointerToHitData(uint8_t* mlt_data, uint64_t* byte_idx, uint64_t hit_byte_idx, int step, uint64_t* maxLeafPointer) {
    if (step == 2) {
        memcpy(&mlt_data[*byte_idx], &hit_byte_idx, 3);
    }
    if (hit_byte_idx > *maxLeafPointer) {
        *maxLeafPointer = hit_byte_idx;
    }
    *byte_idx += 3;
}

void setHitCount(uint8_t* mlt_data, uint64_t* byte_idx, uint64_t count, int step) {
    if (step == 2) {
        memcpy(&mlt_data[*byte_idx], &count, 3);
    }
    // FIXME: Max size of count = 3B
    // assert(count <= 16777215);
    *byte_idx += 3;
}   

void ert_traverse_kmertree_sl(node_t* n, uint8_t* mlt_data, uint8_t* leaf_data, uint64_t* size, uint64_t* leaf_tbl_size, uint8_t depth, uint64_t* numHits, uint64_t* max_ptr, uint64_t next_ptr_width, int step, uint64_t* maxLeafPointer) {
    int j = 0;
    uint8_t cur_depth = depth;
    uint64_t byte_idx = *size;
    uint64_t leaf_byte_idx = *leaf_tbl_size;
    uint8_t code = 0;
    uint8_t mh = 0;
    
    /// Add pointer to leaf table at every internal node
    setPointerToHitData(mlt_data, &byte_idx, leaf_byte_idx, step, maxLeafPointer);
    setHitCount(mlt_data, &byte_idx, n->numHits, step);
    
    assert(n->numChildren != 0);
    if (n->numChildren == 1) {
        node_t* child = n->child_nodes[0];
        uint8_t c = child->seq[child->pos];
        if (child->type == LEAF) {
            // 
            // FIXME: In rare cases, when one of the occurrences of the k-mer is at the end of the reference,
            // # hits for parent node is not equal to the sum of #hits of children nodes, and we trigger the assertion below
            // This should not affect results as long as readLength > kmerSize
            // assert(child->numHits > 1);
            code |= (LEAF << (c << 1));
            mh |= (LEAF << (c << 1));
            addCode(mlt_data, &byte_idx, code, step);
            addMultiHitInfo(mlt_data, &byte_idx, mh, step);
            addMultiHitLeafCount(mlt_data, &byte_idx, child->numHits, step);
            addMultiHitLeafNode(mlt_data, &byte_idx, child->numHits, child->hits, child->leaf_prefix, step);
            addMultiHitLeafNode(leaf_data, &leaf_byte_idx, child->numHits, child->hits, child->leaf_prefix, step);
            *numHits += child->numHits;
        }
        else {
            assert(child->type == UNIFORM);
            code |= (UNIFORM << (c << 1));
            addCode(mlt_data, &byte_idx, code, step);
            addUniformNode(mlt_data, &byte_idx, child->num_bp, &child->seq[child->pos], child->numHits, step);
            ert_traverse_kmertree_sl(child, mlt_data, leaf_data, &byte_idx, &leaf_byte_idx, cur_depth+child->num_bp, 
                                     numHits, max_ptr, next_ptr_width, step, maxLeafPointer);
        }
    }
    else {
        uint8_t numEmpty = 0, numLeaves = 0;
        for (j = 0; j < n->numChildren; ++j) {
            node_t* child = n->child_nodes[j];
            uint8_t c = child->seq[child->pos];
            if (child->type == EMPTY) {
                numEmpty++;
            }
            else if (child->type == LEAF) {
                numLeaves++;
                if (child->numHits > 1) {
                    mh |= (LEAF << (c << 1));
                }
                code |= (LEAF << (c << 1));
            }
            else {
                code |= (DIVERGE << (c << 1));
            }
        }
        uint8_t numPointers = ((4 - numEmpty - numLeaves) > 0) ? (4 - numEmpty - numLeaves) : 0;
        uint64_t start_byte_idx = byte_idx;
        addCode(mlt_data, &byte_idx, code, step);
        if (numLeaves > 0) {
            addMultiHitInfo(mlt_data, &byte_idx, mh, step);
        }
        uint64_t ptr_byte_idx = byte_idx;
        uint64_t ptrToOtherNodes[numPointers + 1]; //!< These point to children. We have one more child than number of pointers
        memset(ptrToOtherNodes, 0, (numPointers + 1)*sizeof(uint64_t));
        uint64_t numHitsForChildren[numPointers + 1];
        memset(numHitsForChildren, 0, (numPointers + 1)*sizeof(uint64_t));
        uint64_t other_idx = 0;
        if (numPointers > 0) {
            byte_idx += (numPointers*next_ptr_width);
        }
        for (j = 0; j < n->numChildren; ++j) {
            node_t* child = n->child_nodes[j];
            if (child->type == LEAF) {
                *numHits += child->numHits;
                if (child->numHits > 1) {
                    addMultiHitLeafCount(mlt_data, &byte_idx, child->numHits, step);
                }
            }
        }
        for (j = 0; j < n->numChildren; ++j) {
            node_t* child = n->child_nodes[j];
            if (child->type == LEAF) {
                if (child->numHits == 1) {
                    addLeafNode(mlt_data, &byte_idx, child->hits[0], child->leaf_prefix, step);
                }
                else {
                    addMultiHitLeafNode(mlt_data, &byte_idx, child->numHits, child->hits, child->leaf_prefix, step);
                }
            }
        }
        if (numPointers > 0) {
            ptrToOtherNodes[other_idx] = byte_idx;
        }
        for (j = 0; j < n->numChildren; ++j) {
            node_t* child = n->child_nodes[j];
            assert(child->type != UNIFORM);
            if (child->type == DIVERGE) {
                ert_traverse_kmertree_sl(child, mlt_data, leaf_data, &byte_idx, &leaf_byte_idx, 
                                         cur_depth+1, numHits, max_ptr, next_ptr_width, step, maxLeafPointer);
                numHitsForChildren[other_idx] = child->numHits; 
                other_idx++;
                ptrToOtherNodes[other_idx] = byte_idx;
            }
            else if (child->type == LEAF) {
                if (child->numHits == 1) {
                    addLeafNode(leaf_data, &leaf_byte_idx, child->hits[0], child->leaf_prefix, step);
                }
                else {
                    addMultiHitLeafNode(leaf_data, &leaf_byte_idx, child->numHits, child->hits, child->leaf_prefix, step);
                }
            }
        }
        for (j = 0; j < numPointers; ++j) {
            uint64_t pointerToNextNode = (ptrToOtherNodes[j] - start_byte_idx);
            if (pointerToNextNode > *max_ptr) {
                *max_ptr = pointerToNextNode;
            }
            assert(pointerToNextNode < (1 << 26));
        }
        /// Fill up pointers based on size of previous children
        if (step == 2) {
            for (j = 0; j < numPointers; ++j) {
                uint64_t pointerToNextNode = (ptrToOtherNodes[j] - start_byte_idx);
                assert(pointerToNextNode < (1 << 26));
                uint64_t reseed_data = 0;
                if (numHitsForChildren[j] < 20) {
                    reseed_data = (pointerToNextNode << 6) | (numHitsForChildren[j]);
                }
                else {
                    reseed_data = (pointerToNextNode << 6);
                }
                memcpy(&mlt_data[ptr_byte_idx], &reseed_data, next_ptr_width);
                ptr_byte_idx += next_ptr_width;
            }
        }
    }
    *size = byte_idx; 
    *leaf_tbl_size = leaf_byte_idx; 
}

void ert_traverse_kmertree(node_t* n, uint8_t* mlt_data, uint8_t* mh_data, uint64_t* size, uint64_t* mh_size, uint8_t depth, uint64_t* numHits, uint64_t* max_ptr, uint64_t next_ptr_width, int step) {
    int j = 0;
    uint8_t cur_depth = depth;
    uint64_t byte_idx = *size;
    uint64_t mh_byte_idx = *mh_size;
    uint8_t code = 0;
    assert(n->numChildren != 0);
    if (n->numChildren == 1) {
        node_t* child = n->child_nodes[0];
        uint8_t c = child->seq[child->pos];
        if (child->type == LEAF) {
            // 
            // FIXME: In rare cases, when one of the occurrences of the k-mer is at the end of the reference,
            // # hits for parent node is not equal to the sum of #hits of children nodes, and we trigger the assertion below
            // This should not affect results as long as readLength > kmerSize
            // assert(child->numHits > 1);
            code |= (LEAF << (c << 1));
            addCode(mlt_data, &byte_idx, code, step);
            addMultiHitLeafPtr(mlt_data, &byte_idx, mh_byte_idx, step);
            addMultiHitLeafCount(mh_data, &mh_byte_idx, child->numHits, step);
            addMultiHitLeafNode(mh_data, &mh_byte_idx, child->numHits, child->hits, child->leaf_prefix, step);
            *numHits += child->numHits;
        }
        else {
            assert(child->type == UNIFORM);
            code |= (UNIFORM << (c << 1));
            addCode(mlt_data, &byte_idx, code, step);
            addUniformNode(mlt_data, &byte_idx, child->num_bp, &child->seq[child->pos], child->numHits, step);
            ert_traverse_kmertree(child, mlt_data, mh_data, &byte_idx, &mh_byte_idx, cur_depth+child->num_bp, 
                                  numHits, max_ptr, next_ptr_width, step);
        }
    }
    else {
        uint8_t numEmpty = 0, numLeaves = 0;
        for (j = 0; j < n->numChildren; ++j) {
            node_t* child = n->child_nodes[j];
            uint8_t c = child->seq[child->pos];
            if (child->type == EMPTY) {
                numEmpty++;
            }
            else if (child->type == LEAF) {
                numLeaves++;
                code |= (LEAF << (c << 1));
            }
            else {
                code |= (DIVERGE << (c << 1));
            }
        }
        uint8_t numPointers = ((4 - numEmpty - numLeaves) > 0) ? (4 - numEmpty - numLeaves) : 0;
        uint64_t start_byte_idx = byte_idx;
        addCode(mlt_data, &byte_idx, code, step);
        uint64_t ptr_byte_idx = byte_idx;
        uint64_t ptrToOtherNodes[numPointers + 1]; //!< These point to children. We have one more child than number of pointers
        memset(ptrToOtherNodes, 0, (numPointers + 1)*sizeof(uint64_t));
        uint64_t numHitsForChildren[numPointers + 1];
        memset(numHitsForChildren, 0, (numPointers + 1)*sizeof(uint64_t));
        uint64_t other_idx = 0;
        if (numPointers > 0) {
            byte_idx += (numPointers*next_ptr_width);
        }
        for (j = 0; j < n->numChildren; ++j) {
            node_t* child = n->child_nodes[j];
            if (child->type == LEAF) {
                if (child->numHits == 1) {
                    addLeafNode(mlt_data, &byte_idx, child->hits[0], child->leaf_prefix, step);
                }
                else {
                    addMultiHitLeafPtr(mlt_data, &byte_idx, mh_byte_idx, step);
                    addMultiHitLeafCount(mh_data, &mh_byte_idx, child->numHits, step);
                    addMultiHitLeafNode(mh_data, &mh_byte_idx, child->numHits, child->hits, child->leaf_prefix, step);
                }
            }
        }
        if (numPointers > 0) {
            ptrToOtherNodes[other_idx] = byte_idx;
        }
        for (j = 0; j < n->numChildren; ++j) {
            node_t* child = n->child_nodes[j];
            assert(child->type != UNIFORM);
            if (child->type == DIVERGE) {
                ert_traverse_kmertree(child, mlt_data, mh_data, &byte_idx, &mh_byte_idx, 
                                      cur_depth+1, numHits, max_ptr, next_ptr_width, step);
                numHitsForChildren[other_idx] = child->numHits; 
                other_idx++;
                ptrToOtherNodes[other_idx] = byte_idx;
            }
        }
        for (j = 0; j < numPointers; ++j) {
            uint64_t pointerToNextNode = (ptrToOtherNodes[j] - start_byte_idx);
            if (pointerToNextNode > *max_ptr) {
                *max_ptr = pointerToNextNode;
            }
            assert(pointerToNextNode < (1 << 26));
        }
        /// Fill up pointers based on size of previous children
        if (step == 2) {
            for (j = 0; j < numPointers; ++j) {
                uint64_t pointerToNextNode = (ptrToOtherNodes[j] - start_byte_idx);
                assert(pointerToNextNode < (1 << 26));
                uint64_t reseed_data = 0;
                if (numHitsForChildren[j] < 20) {
                    reseed_data = (pointerToNextNode << 6) | (numHitsForChildren[j]);
                }
                else {
                    reseed_data = (pointerToNextNode << 6);
                }
                memcpy(&mlt_data[ptr_byte_idx], &reseed_data, next_ptr_width);
                ptr_byte_idx += next_ptr_width;
            }
        }
    }
    *size = byte_idx; 
    *mh_size = mh_byte_idx; 
}

void ert_destroy_kmertree(node_t* n) {
    int j;
    if (n == NULL) {
        return;
    }
    if (n->hits) {
        free(n->hits);
    }
    if (n->leaf_prefix) {
        for (j = 0; j < n->numHits; ++j) {
            free(n->leaf_prefix[j]);
        }
        free(n->leaf_prefix);
    }
    for (j = 0; j < n->numChildren; ++j) {
        ert_destroy_kmertree(n->child_nodes[j]);
    }
    free(n);
}

//
// This function builds the ERT index. 
// Note on pointers to child nodes: When building the radix tree for each k-mer, 
// we try 3 values for pointers to child nodes, 2,3,4 B and choose the smallest
// one possible.
//
void* buildIndex(void *arg) {

    thread_data_t *data = (thread_data_t *)arg;
	bwtintv_t ik, ok[4];
    uint64_t idx = 0;
    uint8_t aq[kmerSize];
    int i; 
    uint8_t c;
    uint64_t lep, prevHits, numBytesPerKmer, numBytesForMh, ref_pos, total_hits = 0, ptr = 0, max_next_ptr = 0;
    uint64_t next_ptr_width = 0; 
    uint64_t nKmerSmallPtr = 0, nKmerMedPtr = 0, nKmerLargePtr = 0;
    uint64_t numBytesForLeaves, leaf_tbl_size = 0;
    uint16_t kmer_data = 0;
    uint8_t large_kmer_tree = 0; // Separate hits for large trees to speed-up leaf gathering
    uint64_t maxLeafPtr = 0;

    /// File to write the multi-level tree index
    char* ml_tbl_file_name = (char*) malloc(strlen(data->filePrefix) + 20);
    sprintf(ml_tbl_file_name, "%s.mlt_table_%d", data->filePrefix, data->tid);
    char* leaf_tbl_file_name = (char*) malloc(strlen(data->filePrefix) + 20);
    sprintf(leaf_tbl_file_name, "%s.leaf_table_%d", data->filePrefix, data->tid);
    /// Log progress
    char* log_file_name = (char*) malloc(strlen(data->filePrefix) + 20);
    sprintf(log_file_name, "%s.log_%d", data->filePrefix, data->tid);
    
    FILE *ml_tbl_fd = 0, *log_fd = 0, *leaf_tbl_fd = 0;
    
    ml_tbl_fd = fopen(ml_tbl_file_name, "wb");
    if (ml_tbl_fd == NULL) {
        printf("\nCan't open file or file doesn't exist. mlt_table errno = %d\n", errno);
        pthread_exit(NULL);
    }
    leaf_tbl_fd = fopen(leaf_tbl_file_name, "wb");
    if (leaf_tbl_fd == NULL) {
        fprintf(stderr, "[M::%s] Can't open file or file doesn't exist. leaf_table errno = %d\n", __func__, errno);
        pthread_exit(NULL);
    } 

    if (bwa_verbose >= 4) {
        log_fd = fopen(log_file_name, "w");
        if (log_fd == NULL) {
            printf("\nCan't open file or file doesn't exist. log errno = %d\n", errno);
            pthread_exit(NULL);
        } 
        log_file(log_fd, "Start: %lu End: %lu", data->startKmer, data->endKmer);
    }

    //
    // Loop for each k-mer and compute LEP when the hit set changes
    //
    for (idx = data->startKmer; idx < data->endKmer; ++idx) {
        max_next_ptr = 0;
        next_ptr_width = 0;
        c = 0;
        lep = 0;   // k-1-bit LEP
        prevHits = 0;
        numBytesPerKmer = 0;
        numBytesForMh = 0;
        numBytesForLeaves = 0;
        maxLeafPtr = 0;
        kmertoquery(idx, aq, kmerSize); // represent k-mer as uint8_t*
        bwt_set_intv(data->bid->bwt, aq[0], ik); // the initial interval of a single base
	    ik.info = 1; 
        prevHits = ik.x[2];
        large_kmer_tree = 0;
        
        // 
        // Backward search k-mer
        //
        for (i = 1; i < kmerSize; ++i) {
            c = 3 - aq[i]; 
			bwt_extend(data->bid->bwt, &ik, ok, 0); // ok contains the result of BWT extension
            if (ok[c].x[2] != prevHits) { // hit set changes
                lep |= (1 << (i-1));
            }
            // 
            // Extend left till k-mer has zero hits
            //
            if (ok[c].x[2] >= 1) { prevHits = ok[c].x[2]; ik = ok[c]; ik.info = i + 1; }
            else { break; }
        }
        
        uint64_t num_hits = ok[c].x[2];
        if (ok[c].x[2] == 0) { // "Empty" - k-mer absent in the reference genome
            kmer_data = ((lep & LEP_MASK) << METADATA_BITWIDTH) | INVALID;
        }
        else if (ok[c].x[2] == 1) { // "Leaf" - k-mer has a single hit in the reference genome
            kmer_data = ((lep & LEP_MASK) << METADATA_BITWIDTH) | (SINGLE_HIT_LEAF);
            numBytesPerKmer = 6;
            uint8_t byte_idx = 0;
            uint8_t mlt_data[numBytesPerKmer];
            if (data->step == 2) { 
                mlt_data[byte_idx] = 0; // Mark that the hit is not a multi-hit
            }
            byte_idx++;
            data->numHits[idx-data->startKmer] += ok[c].x[2];
            if (data->step == 2) {
                //
                // Look up suffix array to identify the hit position
                //
                ref_pos = bwt_sa(data->bid->bwt, ok[c].x[0]);
                //
                // Fetch reference
                //
                int64_t len;
                uint8_t* rseq = bns_get_seq(data->bid->bns->l_pac, data->bid->pac, ref_pos-PREFIX_LENGTH, ref_pos, &len);
                // 
                // FIXME: If len == 0, we are spanning the forward-reverse boundary. We may still need to store a prefix
                //
                if (len > 0) {
                    // 
                    // Add prefix characters for each hit position.
                    //
                    int j;                                                           
                    uint8_t packed_prefix[1];                                        
                    memset(packed_prefix, 0, sizeof(uint8_t));                       
                    for (j = 0; j < PREFIX_LENGTH; ++j) {                                        
                        _set_pac(packed_prefix, j, rseq[j]);               
                    }
                    uint64_t leaf = (ref_pos << 7) | (packed_prefix[0] << 1);
                    memcpy(&mlt_data[byte_idx], &leaf, 5);                  
                    free(rseq);
                }
                fwrite(mlt_data, sizeof(uint8_t), numBytesPerKmer, ml_tbl_fd);
            }
            byte_idx += 5;
        }
        //
        // If the number of hits for the k-mer does not exceed the HIT_THRESHOLD,
        // prefer a radix-tree over a multi-level table as the radix tree for the
        // k-mer is likely to be sparse.
        //
        else if (ok[c].x[2] <= HIT_THRESHOLD) {
            kmer_data = ((lep & LEP_MASK) << METADATA_BITWIDTH) | (INFREQUENT);
            node_t* n = (node_t*) calloc(1, sizeof(node_t));
            n->type = DIVERGE;
            n->pos = 0;
            n->num_bp = 0;
            memcpy(n->seq, aq, kmerSize);
            n->l_seq = kmerSize;
            n->parent_node = 0;
            n->numChildren = 0;
            n->numHits = ok[c].x[2];
            n->child_nodes[0] = n->child_nodes[1] = n->child_nodes[2] = n->child_nodes[3] = 0;
            n->start_addr = 0;
            uint8_t* mlt_data = 0;
            next_ptr_width = 2;
            uint8_t* mh_data = 0;
            uint64_t size = 0;
            uint8_t* leaf_data = 0;
            if (data->step == 1 || data->step == 2) {
                large_kmer_tree = (data->byte_offsets[idx] >> 16) & 1;
            }
            if (data->step == 2) {
                if (idx != (numKmers - 1)) {
                    size = (data->byte_offsets[idx+1] >> KMER_DATA_BITWIDTH) - (data->byte_offsets[idx] >> KMER_DATA_BITWIDTH); 
                    assert(size < (1 << 26));
                } 
                else { // FIXME: This is a hack. We know the size of every k-mer tree except the last-kmer 
                    size = 1 << 26;
                }
                next_ptr_width = (((data->byte_offsets[idx] >> 22) & 3) == 0)? 4 : ((data->byte_offsets[idx] >> 22) & 3);  
                mlt_data = (uint8_t*) calloc(size, sizeof(uint8_t));
                if (large_kmer_tree) {
                    leaf_data = (uint8_t*) calloc(size << 3, sizeof(uint8_t)); //!< We don't know the size of leaf nodes apriori
                }
                else {
                    mh_data = (uint8_t*) calloc(size, sizeof(uint8_t));
                }
            }
            ert_build_kmertree(data->bid->bwt, data->bid->bns, data->bid->pac, ik, ok, i, n, data->step, data->readLength - 1);
            // 
            // Reserve space for pointer to start of multi-hit address space
            //
            numBytesPerKmer = 4; 
            if (large_kmer_tree) {
                ert_traverse_kmertree_sl(n, mlt_data, leaf_data, &numBytesPerKmer, &numBytesForLeaves, 
                                         i, &data->numHits[idx-data->startKmer], &max_next_ptr, next_ptr_width, data->step, &maxLeafPtr);
            }
            else {    
                /// Traverse tree and place data in memory space
                ert_traverse_kmertree(n, mlt_data, mh_data, &numBytesPerKmer, &numBytesForMh, 
                                      i, &data->numHits[idx-data->startKmer], &max_next_ptr, next_ptr_width, data->step);
            }
            if (data->step == 0 || data->step == 1) {
                if (max_next_ptr >= 1024 && max_next_ptr < 262144) {
                    next_ptr_width = 3;
                    max_next_ptr = 0;
                    numBytesPerKmer = 4;
                    numBytesForMh = 0;
                    numBytesForLeaves = 0;
                    if (large_kmer_tree) {
                        ert_traverse_kmertree_sl(n, mlt_data, leaf_data, &numBytesPerKmer, &numBytesForLeaves, i, 
                                                 &data->numHits[idx-data->startKmer], &max_next_ptr, next_ptr_width, data->step, &maxLeafPtr); 
                    }
                    else {
                        ert_traverse_kmertree(n, mlt_data, mh_data, &numBytesPerKmer, &numBytesForMh, i, 
                                              &data->numHits[idx-data->startKmer], &max_next_ptr, next_ptr_width, data->step); 
                    }
                }
                if (max_next_ptr >= 262144) {
                    next_ptr_width = 4;
                    max_next_ptr = 0;
                    numBytesPerKmer = 4;
                    numBytesForMh = 0;
                    numBytesForLeaves = 0;
                    if (large_kmer_tree) {
                        ert_traverse_kmertree_sl(n, mlt_data, leaf_data, &numBytesPerKmer, &numBytesForLeaves, i, 
                                                 &data->numHits[idx-data->startKmer], &max_next_ptr, next_ptr_width, data->step, &maxLeafPtr); 
                    }
                    else {
                        ert_traverse_kmertree(n, mlt_data, mh_data, &numBytesPerKmer, &numBytesForMh, i, 
                                              &data->numHits[idx-data->startKmer], &max_next_ptr, next_ptr_width, data->step); 
                    }
                }
            }
            // log_file(log_fd, "Kmer:%llu,Bytes:%llu", idx, numBytesPerKmer);
            ert_destroy_kmertree(n);
            assert(numBytesPerKmer < (1 << 26));
            assert(numBytesForMh < (1 << 24));
            if (data->step == 2) {
                if (large_kmer_tree) {
                    uint64_t abs_leaf_table_offset = data->leaf_table_offsets + leaf_tbl_size;
                    memcpy(mlt_data, &abs_leaf_table_offset, 4*sizeof(uint8_t));
                    fwrite(mlt_data, sizeof(uint8_t), numBytesPerKmer, ml_tbl_fd);
                    free(mlt_data);
                    fwrite(leaf_data, sizeof(uint8_t), numBytesForLeaves, leaf_tbl_fd);
                    free(leaf_data);
                }
                else {
                    if (idx != numKmers-1) assert((numBytesPerKmer+numBytesForMh) == size);
                    memcpy(mlt_data, &numBytesPerKmer, 4*sizeof(uint8_t));
                    fwrite(mlt_data, sizeof(uint8_t), numBytesPerKmer, ml_tbl_fd);
                    free(mlt_data);
                    fwrite(mh_data, sizeof(uint8_t), numBytesForMh, ml_tbl_fd);
                    free(mh_data);
                }
            }
        }
        // 
        // If the number of hits for the k-mer exceeds the HIT_THRESHOLD,
        // prefer a multi-level table to encode the suffixes for the 
        // k-mer
        //
        else {
            kmer_data = ((lep & LEP_MASK) << METADATA_BITWIDTH) | (FREQUENT); 
            uint8_t* mlt_data = 0;
            uint8_t* mh_data = 0;
            uint8_t* leaf_data = 0;
            next_ptr_width = 2;
            uint64_t size = 0;
            if (data->step == 1 || data->step == 2) {
                large_kmer_tree = (data->byte_offsets[idx] >> 16) & 1;
            }
            if (data->step == 2) {
                if (idx != (numKmers - 1)) {
                    size = (data->byte_offsets[idx+1] >> KMER_DATA_BITWIDTH) - (data->byte_offsets[idx] >> KMER_DATA_BITWIDTH); 
                    assert(size < (1 << 26));
                } 
                else { //!< FIXME: Hack. We do not store the size of the last-kmer
                    size = 1 << 26;
                }
                next_ptr_width = (((data->byte_offsets[idx] >> 22) & 3) == 0)? 4 : ((data->byte_offsets[idx] >> 22) & 3);  
                mlt_data = (uint8_t*) calloc(size, sizeof(uint8_t));
                if (large_kmer_tree) {
                    leaf_data = (uint8_t*) calloc(size << 3, sizeof(uint8_t)); //!< We don't know the size of leaf nodes apriori
                }
                else {
                    mh_data = (uint8_t*) calloc(size, sizeof(uint8_t));
                }
            }
            numBytesPerKmer = 4;
            if (large_kmer_tree) {
                ert_build_table_sl(data->bid->bwt, data->bid->bns, data->bid->pac, ik, ok, mlt_data, leaf_data, &numBytesPerKmer,
                                   &numBytesForLeaves, aq, &data->numHits[idx-data->startKmer], &max_next_ptr, next_ptr_width, data->step,
                                   data->readLength - 1, &maxLeafPtr);
            }
            else {
                ert_build_table(data->bid->bwt, data->bid->bns, data->bid->pac, ik, ok, mlt_data, mh_data, &numBytesPerKmer,
                                &numBytesForMh, aq, &data->numHits[idx-data->startKmer], &max_next_ptr, next_ptr_width, data->step,
                                data->readLength - 1);
            }
            if (data->step == 0 || data->step == 1) {
                if (max_next_ptr >= 1024 && max_next_ptr < 262144) {
                    next_ptr_width = 3;
                    max_next_ptr = 0;
                    numBytesPerKmer = 4;
                    numBytesForMh = 0;
                    numBytesForLeaves = 0;
                    if (large_kmer_tree) {
                        ert_build_table_sl(data->bid->bwt, data->bid->bns, data->bid->pac, ik, ok, mlt_data, leaf_data, 
                                           &numBytesPerKmer, &numBytesForLeaves, aq, &data->numHits[idx-data->startKmer], 
                                           &max_next_ptr, next_ptr_width, data->step, data->readLength - 1, &maxLeafPtr);
                    }
                    else {
                        ert_build_table(data->bid->bwt, data->bid->bns, data->bid->pac, ik, ok, mlt_data, mh_data, 
                                        &numBytesPerKmer, &numBytesForMh, aq, &data->numHits[idx-data->startKmer], 
                                        &max_next_ptr, next_ptr_width, data->step, data->readLength - 1);
                    }
                }
                if (max_next_ptr >= 262144) {
                    next_ptr_width = 4;
                    max_next_ptr = 0;
                    numBytesPerKmer = 4;
                    numBytesForMh = 0;
                    numBytesForLeaves = 0;
                    if (large_kmer_tree) {
                        ert_build_table_sl(data->bid->bwt, data->bid->bns, data->bid->pac, ik, ok, mlt_data, leaf_data, 
                                           &numBytesPerKmer, &numBytesForLeaves, aq, &data->numHits[idx-data->startKmer], 
                                           &max_next_ptr, next_ptr_width, data->step, data->readLength - 1, &maxLeafPtr);
                    }
                    else { 
                        ert_build_table(data->bid->bwt, data->bid->bns, data->bid->pac, ik, ok, mlt_data, mh_data, 
                                        &numBytesPerKmer, &numBytesForMh, aq, &data->numHits[idx-data->startKmer], 
                                        &max_next_ptr, next_ptr_width, data->step, data->readLength - 1);
                    }
                }
            }
            // 
            // Traverse tree and place data in memory
            //
            assert(numBytesPerKmer < (1 << 26));
            assert(numBytesForMh < (1 << 24));
            if (data->step == 2) {
                if (large_kmer_tree) {
                    uint64_t abs_leaf_table_offset = data->leaf_table_offsets + leaf_tbl_size;
                    memcpy(mlt_data, &abs_leaf_table_offset, 4*sizeof(uint8_t));
                    fwrite(mlt_data, sizeof(uint8_t), numBytesPerKmer, ml_tbl_fd);
                    free(mlt_data);
                    fwrite(leaf_data, sizeof(uint8_t), numBytesForLeaves, leaf_tbl_fd);
                    free(leaf_data);
                }
                else {
                    if (idx != numKmers-1) assert((numBytesPerKmer+numBytesForMh) == size);
                    memcpy(mlt_data, &numBytesPerKmer, 4*sizeof(uint8_t));
                    fwrite(mlt_data, sizeof(uint8_t), numBytesPerKmer, ml_tbl_fd);
                    free(mlt_data);
                    fwrite(mh_data, sizeof(uint8_t), numBytesForMh, ml_tbl_fd);
                    free(mh_data);
                }
            }
        }
        if (num_hits < 20) {
            data->kmer_table[idx-data->startKmer] = (ptr << KMER_DATA_BITWIDTH) | (num_hits << 17) | kmer_data;
        }
        else {
            data->kmer_table[idx-data->startKmer] = (ptr << KMER_DATA_BITWIDTH) | kmer_data;
        }
       
        /// If k-mer tree size > DRAM_PAGE_SIZE, add a bit in the k-mer table to indicate
        if (numBytesPerKmer >= DRAM_PAGE_SIZE) {
            data->kmer_table[idx-data->startKmer] |= (1 << 16);
        }

        //
        ptr += (numBytesPerKmer + numBytesForMh);
        leaf_tbl_size += numBytesForLeaves;

        if (next_ptr_width == 2) {
            nKmerSmallPtr++;
        }
        else if (next_ptr_width == 3) {
            nKmerMedPtr++;
        }
        else if (next_ptr_width == 4) {
            nKmerLargePtr++;
            next_ptr_width = 0;
        }

        //
        data->kmer_table[idx-data->startKmer] |= (next_ptr_width << 22);

        if (bwa_verbose >= 4) {
            if (idx == data->endKmer-1) {
                log_file(log_fd, "TotalSize:%lu\n", ptr);
            }
            if ((idx-data->startKmer) % 10000000 == 0) {
                log_file(log_fd, "%lu,%lu,%lu", idx, numBytesPerKmer, ptr);
            }
        }
        
        total_hits += data->numHits[idx-data->startKmer];

        if (maxLeafPtr > data->maxLeafPtr) {
            data->maxLeafPtr = maxLeafPtr;
        }
    }
    
    //
    data->end_offset = ptr;
    data->leaf_table_offsets = leaf_tbl_size;

    if (bwa_verbose >= 4) {
        log_file(log_fd, "Hits:%lu\n", total_hits);
        log_file(log_fd, "nKmersSmallPtrs:%lu", nKmerSmallPtr);
        log_file(log_fd, "nKmersMedPtrs:%lu", nKmerMedPtr);
        log_file(log_fd, "nKmersLargePtrs:%lu", nKmerLargePtr);
        fclose(log_fd);
    }
    fclose(ml_tbl_fd);
    fclose(leaf_tbl_fd);
    free(ml_tbl_file_name);
    free(leaf_tbl_file_name);
    free(log_file_name);
    pthread_exit(NULL);
}

void buildKmerTrees(char* kmer_tbl_file_name, bwaidx_t* bid, char* prefix, int num_threads, int readLength) {
    FILE* kmer_tbl_fd;
    pthread_t thr[num_threads];
    int i, rc;
    thread_data_t thr_data[num_threads];
    uint64_t numKmersThread = (uint64_t)ceil(((double)(numKmers))/num_threads);
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s] Computing tree sizes for each k-mer\n", __func__);
    }
    // 
    // STEP 1: Create threads. Each thread builds the index for a fraction of the k-mers
    //
    for (i = 0; i < num_threads; ++i) {
        thr_data[i].tid = i;
        thr_data[i].step = 0;
        thr_data[i].readLength = readLength;
        thr_data[i].bid = bid; 
        thr_data[i].startKmer = i*numKmersThread;
        thr_data[i].endKmer = ((i + 1)*numKmersThread > numKmers) ? numKmers : (i + 1)*numKmersThread;  
        thr_data[i].end_offset = 0;
        thr_data[i].filePrefix = prefix;
        thr_data[i].maxLeafPtr = 0;
        uint64_t numKmersToProcess = thr_data[i].endKmer - thr_data[i].startKmer;
        thr_data[i].kmer_table = (uint64_t*) calloc(numKmersToProcess, sizeof(uint64_t));
        thr_data[i].numHits = (uint64_t*) calloc(numKmersToProcess, sizeof(uint64_t));
        if ((rc = pthread_create(&thr[i], NULL, buildIndex, &thr_data[i]))) {
            fprintf(stderr, "[M::%s] error: pthread_create, rc: %d\n", __func__, rc);
            return;
        }
    }
    // 
    // block until all threads complete
    //
    for (i = 0; i < num_threads; ++i) {
        pthread_join(thr[i], NULL);
    }

    //
    // Compute absolute offsets for each kmer's tree from per-thread relative offsets
    //
    uint64_t* kmer_table = (uint64_t*) calloc(numKmers, sizeof(uint64_t));
    uint64_t tidx, kidx;
    uint64_t numProcessed = 0;
    uint64_t offset = 0;
    for (tidx = 0; tidx < num_threads; ++tidx) {
        uint64_t numKmersToProcess = thr_data[tidx].endKmer - thr_data[tidx].startKmer;
        for (kidx = 0; kidx < numKmersToProcess; ++kidx) {
            uint64_t rel_offset = thr_data[tidx].kmer_table[kidx] >> KMER_DATA_BITWIDTH;
            uint16_t kmer_data = thr_data[tidx].kmer_table[kidx] & KMER_DATA_MASK;
            uint64_t ptr_width = (thr_data[tidx].kmer_table[kidx] >> 22) & 3;
            uint64_t reseed_hits = (thr_data[tidx].kmer_table[kidx] >> 17) & 0x1F;
            uint64_t large_kmer_tree = (thr_data[tidx].kmer_table[kidx] >> 16) & 1;;
            kmer_table[numProcessed + kidx] =   ((offset + rel_offset) << KMER_DATA_BITWIDTH) 
                                                | (ptr_width << 22) 
                                                | (reseed_hits << 17) 
                                                | (large_kmer_tree << 16) 
                                                | (kmer_data);        
        }
        numProcessed += numKmersToProcess;
        offset += thr_data[tidx].end_offset;
        free(thr_data[tidx].kmer_table);
        free(thr_data[tidx].numHits);
    }
    
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s] Re-estimating sizes after separating leaves for large trees ...\n", __func__);
    }

    // 
    // STEP 2: Re-build index after estimating size for large trees. K-mers with large trees use 
    // a different memory layout
    //
    for (i = 0; i < num_threads; ++i) {
        thr_data[i].tid = i;
        thr_data[i].step = 1;
        thr_data[i].readLength = readLength;
        thr_data[i].bid = bid; 
        thr_data[i].startKmer = i*numKmersThread;
        thr_data[i].endKmer = ((i + 1)*numKmersThread > numKmers) ? numKmers : (i + 1)*numKmersThread;  
        thr_data[i].end_offset = 0;
        thr_data[i].filePrefix = prefix;
        thr_data[i].maxLeafPtr = 0;
        uint64_t numKmersToProcess = thr_data[i].endKmer - thr_data[i].startKmer;
        thr_data[i].kmer_table = (uint64_t*) calloc(numKmersToProcess, sizeof(uint64_t));
        thr_data[i].numHits = (uint64_t*) calloc(numKmersToProcess, sizeof(uint64_t));
        thr_data[i].byte_offsets = kmer_table;
        if ((rc = pthread_create(&thr[i], NULL, buildIndex, &thr_data[i]))) {
            fprintf(stderr, "[M::%s] error: pthread_create, rc: %d\n", __func__, rc);
            return;
        }
    }
    
    for (i = 0; i < num_threads; ++i) {
        pthread_join(thr[i], NULL);
    }
   
    //
    // Compute absolute offsets for each k-mer tree's root node
    // 
    numProcessed = 0;
    offset = 0; 
    uint64_t maxLeafPtr = 0;
    for (tidx = 0; tidx < num_threads; ++tidx) {
        uint64_t numKmersToProcess = thr_data[tidx].endKmer - thr_data[tidx].startKmer;
        for (kidx = 0; kidx < numKmersToProcess; ++kidx) {
            uint64_t rel_offset = thr_data[tidx].kmer_table[kidx] >> KMER_DATA_BITWIDTH;
            uint16_t kmer_data = thr_data[tidx].kmer_table[kidx] & KMER_DATA_MASK;
            uint64_t ptr_width = (thr_data[tidx].kmer_table[kidx] >> 22) & 3;
            uint64_t reseed_hits = (thr_data[tidx].kmer_table[kidx] >> 17) & 0x1F;
            uint64_t large_kmer_tree = (kmer_table[numProcessed + kidx] >> 16) & 1;
            kmer_table[numProcessed + kidx] =   ((offset + rel_offset) << KMER_DATA_BITWIDTH) 
                                                | (ptr_width << 22) 
                                                | (reseed_hits << 17) 
                                                | (large_kmer_tree << 16)
                                                | (kmer_data);        
        }
        numProcessed += numKmersToProcess;
        offset += thr_data[tidx].end_offset;
        free(thr_data[tidx].kmer_table);
        free(thr_data[tidx].numHits);
        if (thr_data[tidx].maxLeafPtr > maxLeafPtr) {
            maxLeafPtr = thr_data[tidx].maxLeafPtr;
        }
    }
    fprintf(stderr, "[M::%s] Max leaf ptr %lu ...\n", __func__, maxLeafPtr);
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s] Building index ...\n", __func__);
    }

    // 
    // STEP 3 : Using estimates of each k-mer's tree size from the previous step, write the index to file
    // 
    uint64_t cum_leaf_table_offset[num_threads + 1];
    cum_leaf_table_offset[0] = 0; 
    for (i = 1; i < num_threads + 1; ++i) {
        cum_leaf_table_offset[i] = cum_leaf_table_offset[i-1] + thr_data[i-1].leaf_table_offsets;
    }
   
    uint64_t total_size = offset + cum_leaf_table_offset[num_threads] + (numKmers * 8UL);
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s] Total size of ERT index = %lu B (Expected). (k-mer,MLT,Leaf) = (%lu,%lu,%lu)\n", __func__, total_size, numKmers * 8UL, offset, cum_leaf_table_offset[num_threads]);
    }    
    
    for (i = 0; i < num_threads; ++i) {
        thr_data[i].tid = i;
        thr_data[i].step = 2;
        thr_data[i].readLength = readLength;
        thr_data[i].bid = bid; 
        thr_data[i].startKmer = i*numKmersThread;
        thr_data[i].endKmer = ((i + 1)*numKmersThread > numKmers) ? numKmers : (i + 1)*numKmersThread;  
        thr_data[i].end_offset = 0;
        thr_data[i].filePrefix = prefix; 
        uint64_t numKmersToProcess = thr_data[i].endKmer - thr_data[i].startKmer;
        thr_data[i].kmer_table = (uint64_t*) calloc(numKmersToProcess, sizeof(uint64_t));
        thr_data[i].numHits = (uint64_t*) calloc(numKmersToProcess, sizeof(uint64_t));
        thr_data[i].byte_offsets = kmer_table;
        thr_data[i].leaf_table_offsets = cum_leaf_table_offset[i];
        if ((rc = pthread_create(&thr[i], NULL, buildIndex, &thr_data[i]))) {
            fprintf(stderr, "[M::%s] error: pthread_create, rc: %d\n", __func__, rc);
            return;
        }
    }
    
    for (i = 0; i < num_threads; ++i) {
        pthread_join(thr[i], NULL);
    }

    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s] Merging per-thread tables ...\n", __func__);
    }
  
    //
    // Compute absolute offsets for each k-mer tree's root node
    //  
    numProcessed = 0;
    offset = 0; 
    for (tidx = 0; tidx < num_threads; ++tidx) {
        uint64_t numKmersToProcess = thr_data[tidx].endKmer - thr_data[tidx].startKmer;
        for (kidx = 0; kidx < numKmersToProcess; ++kidx) {
            uint64_t rel_offset = thr_data[tidx].kmer_table[kidx] >> KMER_DATA_BITWIDTH;
            uint16_t kmer_data = thr_data[tidx].kmer_table[kidx] & KMER_DATA_MASK;
            uint64_t ptr_width = (thr_data[tidx].kmer_table[kidx] >> 22) & 3;
            uint64_t reseed_hits = (thr_data[tidx].kmer_table[kidx] >> 17) & 0x1F;
            uint64_t large_kmer_tree = (kmer_table[numProcessed + kidx] >> 16) & 1;
            kmer_table[numProcessed + kidx] =   ((offset + rel_offset) << KMER_DATA_BITWIDTH) 
                                                | (ptr_width << 22) 
                                                | (reseed_hits << 17) 
                                                | (large_kmer_tree << 16)
                                                | (kmer_data);        
        }
        numProcessed += numKmersToProcess;
        offset += thr_data[tidx].end_offset;
        free(thr_data[tidx].kmer_table);
        free(thr_data[tidx].numHits);
    }
    kmer_tbl_fd = fopen(kmer_tbl_file_name, "wb");
    if (kmer_tbl_fd == NULL) {
        fprintf(stderr, "[M::%s] Can't open file or file doesn't exist.\n", __func__);
        exit(1);
    }
    fwrite(kmer_table, sizeof(uint64_t), numKmers, kmer_tbl_fd);
    fclose(kmer_tbl_fd);
    free(kmer_table);
}
