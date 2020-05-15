//
//  COO.h
//
//

#ifndef COO_h
#define COO_h

#include <algorithm>
#include <iostream>
#include <cinttypes>
#include <random>
#include "defs.h"
#include "utils.h"
#include "GAP/timer.h"
#include "GAP/util.h"
#include "GAP/pvector.h"
#include "GAP/platform_atomics.h"



// RIT: Row Index Type
// CIT: Column Index Type
// VT: Value Type
// If unweighted, nzVals_ is of zero length
// We used three arrays for indices and valued so that we can access them independtly
// This choice would reduce data trafic such as in symbolic steps
// However, it will make sorting harder to do
// We will implement our own sorting using histogram sort (similar to propagation blocking)
template <typename RIT, typename CIT, typename VT=double>
class COO
{
public:
    COO(): nrows_(0), ncols_(0), nnz_(0), sortType_(UNSORTED) {}
    //COO(RIT nrows, CIT ncols, int64_t nnz, bool isWeighted): nrows_(nrows), ncols_(ncols), nnz_(nnz), sortType_(UNSORTED), isWeighted_(isWeighted) {NzList.resize(nnz_);}
    
    
    //------------------------------------------------
    // Accessor methods for indices and values
    // TODO: how should we check the validity of the access without sacrifycing the performance
    //       C++ vector will generate errors
    // TODO: should we use iterators? How is the parallel performace of iterators
    // The size of nzRows/nzRows/nzVals is nnz_
    // hence, index i cannot be RIT or CIT
    //------------------------------------------------
    // Get
    RIT NzRows (size_t i) const { return nzRows_[i]; }
    CIT NzCols (size_t i) const { return nzCols_[i]; }
    VT NzVals (size_t i) const { return nzVals_[i]; }
    
    size_t nrows() const {return nrows_;}
    size_t ncols() const {return ncols_;}
    size_t nnz() const {return nnz_;}
    bool isWeighted() const{return isWeighted_;}
    
    // Get references
    // TODO: why do we need this? Better to use setting function
    RIT & NzRows (size_t i) { return nzRows_[i]; }
    CIT & NzCols (size_t i) { return nzCols_[i]; }
    VT & NzVals (size_t i) { return nzVals_[i]; }
    
    
    void GenER(int scale, int d, bool isWeighted, int64_t kRandSeed = 5102020);
    void GenRMAT(int scale, int d, bool isWeighted, int64_t kRandSeed = 5102020);
    void RandReLabel(bool symmPerm=true);
    pvector<CIT>  NnzPerRow();
    pvector<RIT>  NnzPerCol();
    void PrintInfo();
    
    template<typename CPT>
    void BinByCol(pvector<CPT>& colPtr, pvector<RIT>& rowIdsBinned, pvector<VT>& nzValsBinned);

    
private:
    size_t nrows_;
    size_t ncols_;
    size_t nnz_;
    pvector<RIT> nzRows_;
    pvector<CIT> nzCols_;
    pvector<VT> nzVals_;
    bool isWeighted_;
    int sortType_;
};

template <typename RIT, typename CIT, typename VT>
void COO<RIT, CIT, VT>::PrintInfo()
{
    std::cout << "COO matrix: " << " Rows= " << nrows_  << " Columns= " << ncols_ << " nnz= " << nnz_ << std::endl;
}

template <typename RIT, typename CIT, typename VT>
pvector<CIT> COO<RIT, CIT, VT>:: NnzPerRow()
{
    pvector<CIT> nnzPerRow(nrows_, 0);
#pragma omp parallel for
    for (auto it = nzRows_.begin(); it < nzRows_.end(); it++)
    {
        RIT rowid = *it;
        fetch_and_add(nnzPerRow[rowid], 1);
    }
    return nnzPerRow;
}


template <typename RIT, typename CIT, typename VT>
pvector<RIT> COO<RIT, CIT, VT>:: NnzPerCol()
{
    pvector<RIT> nnzPerCol(nrows_, 0);
#pragma omp parallel for
    for (auto it = nzCols_.begin(); it < nzCols_.end(); it++)
    {
        CIT colid = *it;
        fetch_and_add(nnzPerCol[colid], 1);
    }
    return nnzPerCol;
}



// Bin by column
// Populate CSC-style data structures
// TODO: use propagation blocking for binning
template <typename RIT, typename CIT, typename VT>
template<typename CPT>
void COO<RIT, CIT, VT>:: BinByCol(pvector<CPT>& colPtr, pvector<RIT>& rowIdsBinned, pvector<VT>& nzValsBinned)
{
    pvector<RIT> nnzPerCol = NnzPerCol();
    ParallelPrefixSum(nnzPerCol, colPtr);
    pvector<CPT> curPtr(colPtr.begin(), colPtr.end());
    rowIdsBinned.resize(nnz_);
    if(isWeighted_)
        nzValsBinned.resize(nnz_);
#pragma omp parallel for
    for(size_t i=0; i<nnz_; i++)
    {
        size_t pos = fetch_and_add(curPtr[nzCols_[i]], 1);
        rowIdsBinned[pos] = nzRows_[i];
        if(isWeighted_) nzValsBinned[pos] = nzVals_[i];
    }
}


template <typename RIT, typename CIT, typename VT>
void COO<RIT, CIT, VT>::GenER(int scale, int d, bool isWeighted, int64_t kRandSeed)
{
    isWeighted_ = isWeighted;
    nrows_ = 1l << scale;
    ncols_ = nrows_;
    nnz_ = nrows_  * d;
   
    //TODO: think about a block size based on the number of threads
    // blocking is done for random number generators
    // one seed per block
    static const size_t block_size = 1<<16;
    
    Timer t;
    t.Start();
    nzRows_.resize(nnz_);
    nzCols_.resize(nnz_);
    
    
    if(isWeighted_) nzVals_.resize(nnz_);
#pragma omp parallel
    {
        std::mt19937 rng;
        std::uniform_int_distribution<RIT> udist(0, nrows_-1);

#pragma omp for
        for (size_t block=0; block < nnz_; block+=block_size)
        {
            rng.seed(kRandSeed + block/block_size);
            for (size_t i=block; i < std::min(block+block_size, nnz_); i++)
            {
                nzRows_[i] = udist(rng);
                nzCols_[i] = udist(rng);
                if(isWeighted_) nzVals_[i] = static_cast<VT>(udist(rng)%255);
            }
        }
    }
    t.Stop();
    PrintTime("ER Generation Time", t.Seconds());
}


// Relabel row and column indices
// This function can destroy block structures in the matrix
// However, dense rows, columns and diagonal remain intact
// TODO: Rewrite a version that combines sorting.
// TODO: Access to permute array can be out of cache (consider blocking)

template <typename RIT, typename CIT, typename VT>
void COO<RIT, CIT, VT>:: RandReLabel(bool symmPerm)
{
    sortType_ = UNSORTED; // not sorted anymore after relabeling
    pvector<RIT> rowPerm(nrows_);
    std::mt19937 rng(ROW_PERM_SEED);
#pragma omp parallel for
    for (RIT i=0; i < nrows_; i++)
        rowPerm[i] = i;
    shuffle(rowPerm.begin(), rowPerm.end(), rng);
    
    // symmetric purmutation
    if((nrows_ == ncols_) &&  symmPerm)
    {

#pragma omp parallel for
        for (size_t i=0; i < nnz_; i++)
        {
            nzRows_[i] = nzRows_[rowPerm[nzRows_[i]]];
            nzCols_[i] = nzCols_[rowPerm[nzCols_[i]]];
        }
    }
    else // unsymmetric purmutation (only for bipartite matrices (square and nonsquare))
    {
        pvector<RIT> colPerm(ncols_);
        std::mt19937 crng(COL_PERM_SEED);
#pragma omp parallel for
        for (CIT i=0; i < ncols_; i++)
            colPerm[i] = i;
        shuffle(colPerm.begin(), colPerm.end(), crng);
#pragma omp parallel for
        for (size_t i=0; i < nnz_; i++)
        {
            nzRows_[i] = nzRows_[rowPerm[nzRows_[i]]];
            nzCols_[i] = nzCols_[colPerm[nzCols_[i]]];
        }
    }
    
}

template <typename RIT, typename CIT, typename VT>
void COO<RIT, CIT, VT>::GenRMAT(int scale, int d, bool isWeighted, int64_t kRandSeed)
{
    isWeighted_ = isWeighted;
    nrows_ = 1l << scale;
    ncols_ = nrows_;
    nnz_ = nrows_  * d;

    static const size_t block_size = 1<<16;
    
    Timer t;
    t.Start();
    
    nzRows_.resize(nnz_);
    nzCols_.resize(nnz_);
    const float A = 0.57f, B = 0.19f, C = 0.19f;

#pragma omp parallel
    {
        std::mt19937 rng;
        std::uniform_real_distribution<float> udist(0, 1.0f);
#pragma omp for
        for (size_t block=0; block < nnz_; block+=block_size)
        {
            rng.seed(kRandSeed + block/block_size);
            for (size_t i=block; i < std::min(block+block_size, nnz_); i++)
            {
                RIT row = 0, col = 0;
                for (int depth=0; depth < scale; depth++) {
                    float rand_point = udist(rng);
                    row = row << 1;
                    col = col << 1;
                    if (rand_point < A+B) {
                        if (rand_point > A)
                            col++;
                    } else {
                        row++;
                        if (rand_point > A+B+C)
                            col++;
                    }
                }
                nzRows_[i] = row;
                nzCols_[i] = col;
            }
        }
    }
    RandReLabel();

    if(isWeighted_)
    {
        nzVals_.resize(nnz_);
#pragma omp parallel
        {
            std::mt19937 rng;
            std::uniform_int_distribution<int> udist(1, 255);
#pragma omp for
            for (size_t block=0; block < nnz_; block+=block_size) {
                rng.seed(kRandSeed + block/block_size);
                for (size_t i=block; i < std::min(block+block_size, nnz_); i++)
                {
                    nzVals_[i] = static_cast<VT>(udist(rng)%255);
                }
            }
        }
    }

    t.Stop();
    PrintTime("RMAT Generation Time", t.Seconds());
}




#endif /* COO_h */