// bradley mackey
// gpumccc assignment 2018/19

#include "utils.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
// this project makes use of 'uthash' for fast dictionary-like lookups
// http://troydhanson.github.io/uthash/
#include "uthash.h"

// swap macro thanks to SO user u0b34a0f6ae: https://stackoverflow.com/a/14300503/3261161
#define SWAP(X,Y) \
    do { \
        unsigned char _buf[sizeof(*(X))]; \
        memmove(_buf, (X), sizeof(_buf)); \
        memmove((X),  (Y), sizeof(_buf)); \
        memmove((Y), _buf, sizeof(_buf)); \
    } while (0)

#define SHOULD_PROFILE 0

#if SHOULD_PROFILE
#include <likwid.h>
#endif

/*
 * FOR ALL OPERATIONS:
 * 0 = row
 * 1 = col
 */


// use `likwid` on Hamilton in order to measure performance of the routines

// skeleton implementation from other files
// will be able to be seen during compilation
void basic_sparsemm(const COO, const COO, COO *);
void basic_sparsemm_sum(const COO, const COO, const COO,
                        const COO, const COO, const COO,
                        COO *);

struct hash_int {
    int column; // key
    int offset; // value
    UT_hash_handle hh; /* makes this structure hashable */
};

/* creates a hash table we can use to store int values */
struct hash_int *make_hash_table() {
    struct hash_int *hash_table = NULL;
    return hash_table;
}

/* adds a value to the hash table */
void add_hash_int(struct hash_int **hash_table, int col_id, int offset) {
    struct hash_int *hi;
    hi = malloc(sizeof(struct hash_int));
    hi->column = col_id;
    hi->offset = offset;
    HASH_ADD_INT( *hash_table, column, hi );  /* id: name of key field */
}

/* finds a given int in the hash table */
struct hash_int *find_offset_from_hash_table(struct hash_int **hash_table, int column) {
    struct hash_int *hi;
    HASH_FIND_INT( *hash_table, &column, hi );  /* s: output pointer */
    return hi;
}

/* tears down hash table, deallocating all memory */
void clear_hash_table(struct hash_int **hash_table) {
  struct hash_int *current_col, *tmp;

  HASH_ITER(hh, *hash_table, current_col, tmp) {
    HASH_DEL(*hash_table,current_col);  /* delete; users advances to next */
    free(current_col);            /* optional- if you want to free  */
  }
}

/* transposes any COO (does not have to be ordered or anything) */
static void transpose_coo_matrix(const COO *M) {

    /* transpose header */
    int tmp;
    tmp = (*M)->m;
    (*M)->m = (*M)->n;
    (*M)->n = tmp;

    /* transpose coordinate values */
    /* data values stay put, because this is a transpose after all! */
    /* can be parallelised */
    int i;
    #pragma acc kernels
    for (i = 0; i<(*M)->NZ; i++) {
        tmp = (*M)->coords[i].i;
        (*M)->coords[i].i = (*M)->coords[i].j;
        (*M)->coords[i].j = tmp;
    }

}


/* function to order a COO by ROW, then COL */
static int order_coo_rows(const void *v1, const void *v2) {
    /* pointer indirection, so we must initially dereference */
    struct coord *c1 = *(struct coord**)v1;
    struct coord *c2 = *(struct coord**)v2;
    int row_comp = c1->i - c2->i;
    if (row_comp != 0) return row_comp;
    // if rows are the same, order by column instead
    return ( c1->j - c2->j );
}

/* compare coordinates FOR BINARY SEARCH ONLY when sorted by ROW, COL */
static int bin_compare_rows(const void *v1, const void *v2) {
    struct coord *c1 = (struct coord*)v1;
    struct coord *c2 = (struct coord*)v2;
    /* compare COLS because we should be in same ROW anyway */
    return ( c1->j - c2->j );
}


/* sorts elements in a COO such that we are ordered by row, then sub-ordered by column
 * pointer array sorted so we can order both the coordinate and data the same way
 * (must be done this way because data and coordinates are stored in separate arrays, and we want them BOTH to be sorted according to the ordering of the coordinates)
 * technique thanks to: https://stackoverflow.com/a/32954558/3261161
 * time complexity: O(nlogn)
 * space complexity: O(n)
 */
// 
static void order_coo_matrix_rows(COO M) {
    
    /* an array that contains pointers to coordinates, these are sorted as a layer of indirection */
    // TODO: sort on the stack if at all possible? -> so much faster (SO for large COOs)
    struct coord **pointer_arr = (struct coord**)malloc(M->NZ*sizeof(struct coord*));
    //struct coord *pointer_arr[M->NZ];
    
    /* create array of pointers to coords[] */
    int i;
    #pragma acc kernels
    #pragma unroll (4)
    for(i = 0; i < M->NZ; i++)
        pointer_arr[i] = &(M->coords[i]);
    
    /* sort array of pointers */
    qsort(pointer_arr, M->NZ, sizeof(struct coord *), order_coo_rows);
    
    /* reorder coords[] and data[] according to the array of pointers
       - sorting is done in place to save memory */
    struct coord tc;
    double td;
    int k, j;
    for(i = 0; i < M->NZ; i++){
        if(i != pointer_arr[i]-(M->coords)){
            tc = M->coords[i];
            td = M->data[i];
            k = i;
            while(i != (j = pointer_arr[k]-(M->coords))){
                M->coords[k] = M->coords[j];
                M->data[k] = M->data[j];
                pointer_arr[k] = &(M->coords[k]);
                k = j;
            }
            M->coords[k] = tc;
            M->data[k] = td;
            pointer_arr[k] = &(M->coords[k]);
        }
    }
    
    /* pointer array no longer needed */
   free(pointer_arr);

}


// finds a matching entry in the other matrix from the lookup table and then by binary search, returning the value of this element.
// if zero_out is set, the value will be zeroed out after access
// function is able to deal with COO files containing 0 entries
// if entry is not able to be found, the `*not_found_flag` will be SET TO 1, and we return 0
// if we found an entry that is actually 0, we return 0 but the flag is NOT altered
// if ROWS, we use row offset table, if COLS, we use col offset table
static double locate_matching_entry_rows(COO M, int *row_offset_table, struct coord to_find, char zero_out, char *not_found_flag) {
    
    // check that there is a matching row in this matrix
    const int row_offset = row_offset_table[to_find.i];
    if (row_offset == -1) {
        *not_found_flag = 1;
        return 0.0f;
    }
    
    const int num_rows = M->m;
    int row_offset_end = -1;
    int k; // the last offset to check (so we know what range the row takes up)
    /* must be serial */
    for (k = (to_find.i+1); k < num_rows; k++) {
        row_offset_end = row_offset_table[k];
        // if not -1 we have found an offset where we should step function
        if (row_offset_end != -1) break;
    }
    /* if val is still -1, we should traverse all the way to the end of the list */
    if (row_offset_end == -1) {
        row_offset_end = M->NZ;
    }

    /* binary search to find the matching column value */
    struct coord *col_ptr = (struct coord*)bsearch(&to_find, row_offset+(M->coords), row_offset_end-row_offset, sizeof(struct coord), bin_compare_rows);
    if (col_ptr == NULL) {
        *not_found_flag = 1;
        return 0.0f;
    }

    // find the offset so we can get at the data value now
    const long index = col_ptr-(M->coords);
    const double result = M->data[index]; // data at the same index coordinates are at, so this is the data value
    if (zero_out) {
        M->data[index] = 0.0f;
    }
    return result;
    
}

/*
 * preprocessing to allow for fast lookup of B elements by index
 * returns an array of the offsets of the elements in B, by row.
 * this allows us to easily find a row, because we know it's offset in the COO list
 * this function does the calculation of that.
 * e.g. where i = row num, j = col num and tuples are (i,j,val)
 * [(0,0,999),(0,1,999),(0,2,999),(1,0,999),(1,1,999),(2,0,999),(4,1,999)]
 * we would get:
 * [0,3,5,-1,6]
 * -1 returned if there is no row for that index value
 * here, there is no row `3` in the matrix, index 3 in result is `-1`, indicating no row
 * time complexity: O(n)
 * space complexity: O(n)
 */
static int *row_offset_table(const COO M) {
    
    const int num_rows = M->m;
    
    /* offset table result */
    int *const result = (int*)malloc(num_rows*sizeof(int));

    // initialise all values to -1
    // this is the value for 'no row', so we can skip over them in the next step
    int i;
    #pragma acc kernels
    #pragma unroll (8)
    for (i = 0; i < num_rows; i++)
        result[i] = -1;
    
    // keep track of row currently being seen and prior rows
    // used to know when we have already filled in offset for a particular row value
    int curr_row, prev_row;
    curr_row = 0;
    prev_row = -1;
    
    int k;
    /* must be executed serially due to offset calc */
    #pragma nounroll
    for (k = 0; k < M->NZ; k++) {

        // the row number for this coordinate
        curr_row = M->coords[k].i;
        
        // if we have not marked the start of this row already...
        if (curr_row != prev_row) {
            // mark the index of where this row starts
            result[curr_row] = k;
            // ensure that we do not update value again, and waste valuable computation
            prev_row = curr_row;
        }
        
    }
    
    return result;
}

/* merges multiple partial row COOs into 1 single COO */
// m is the number of rows result will have n is the number of columns result will have 
static void merge_result_rows(int num_rows, int m, int n, COO * coo_list, COO * final) {

    /* final should point to result when we are done */
    *final = NULL;

    /* result will be the first entry in the matrix (we will append other items to this) */
    COO result;

    /* memory offsets so we know what offset to memcpy to */
    int *memory_offsets = (int*)malloc(num_rows*sizeof(int));
    int total_rows = 0;
    int total_items = 0;
    int i;
    COO coo;
    /* must be executed sequentially due to depencies on prior offsets */
    for (i=0;i<num_rows;i++) {
        coo = coo_list[i];
        if (coo == NULL) {
            /* if this row does not exist, there is no offset */
            memory_offsets[i] = -1;
        } else {
            /* if this is the first row we have encountered, it will be the one we append everything to */
            if (total_rows == 0) result = coo;
            /* update memory offset for this row */
            memory_offsets[i] = total_items;
            total_items += coo->NZ;
            total_rows++;
        }
    }

    /* allocate all the memory that we will need (so we pay less of a performance cost later)
     * rather than reallocating the memory to fit for each row that we add, we allocate it all right here and now! */
    result->coords = (struct coord*)realloc(result->coords,total_items*sizeof(struct coord));
    result->data = (double *)realloc(result->data,total_items*sizeof(double));
    result->NZ = total_items;
    result->m = m;
    result->n = n;

    // printf("-----> result before merge <--- \n");
    // print_sparse(result);

    struct coord *coord_ptr;
    double *data_ptr;
    int mem_offset;
    /* all rows can be merged parallel, as they are all copied to distinct memory locations */
    #pragma acc kernels
    for (i = 0; i<num_rows; i++) {
        /* if there is no offset (-1), there is no row */
        /* if row is 0, this is the first row, and it's result is already in the result! */
        mem_offset = memory_offsets[i];
        if (mem_offset <= 0)
            continue;

        /* add coordinates and data to the end of the current list */
        coord_ptr = (result->coords)+mem_offset;
        data_ptr = (result->data)+mem_offset;
        coo = coo_list[i];
        memcpy(coord_ptr,coo->coords,coo->NZ*sizeof(struct coord));
        memcpy(data_ptr,coo->data,coo->NZ*sizeof(double));

        // printf("-----> merged at round %d <--- \n",i);
        // print_sparse(result);

        /* free the old, unneeded old list (it is now in `result` so we need it no longer) */
        free_sparse(&coo);
    }

    free(memory_offsets);

//    printf(" --- RESULT MATRIX ---:\n");
//    print_sparse(result);
    *final = result;

}

/* calculates a result row in the resultant matrix 
   - row_res will allocated by this routine */
static void calculate_result_row(int a_row, COO A, int *a_row_offsets, COO B, int *b_row_offsets, COO *row_res) {

    /* how big this resultant row will be */
    const int num_cols_a = A->n;

    int a_row_offset = a_row_offsets[a_row];
    if (a_row_offset == -1) {
        // if there is no offset for this row of A
        // then there will be no result for this row
        *row_res = NULL;
        return;
    }


    /* estimate for the number of non-zeros for this resultant row */
    /* obviously we cannot know before, so calculate this initially, */
    /* then we can realloc if we run out of room */
    /* we divide by number of rows in result because we are only calculating per row remember! */
    const int initial_rows = (int)((A->NZ+B->NZ)*2/A->m);
    int current_allocated_size = initial_rows;
    
    
    // bear in mind this only represents a single row of the resultant matrix
    // this is the values for only one row of this result
    /* FINAL 1 row total (because this is just a result row) */
    alloc_sparse(1,num_cols_a,current_allocated_size,row_res);

    /* the start position of each new element in each new row, so we know where to look to add multiple things */
    struct hash_int *pass_start_positions_for_col = make_hash_table();
    struct hash_int *existing_offset;

    COO row = *row_res;

    int non_zero_elements = 0;

    /* iterate over elements of A in the specified row */
    int b_row, a_col, b_col; // track real row and col positions in the would-be full matrix
    int a_itr, b_itr; // itr variables keep track of positions in the COO files
    int b_row_offset;
    double result, prev_val;
    /* loop will overshoot, break when needed */
    #pragma acc kernels loop
    for (a_itr = 0; a_itr < num_cols_a; a_itr++) {

        /* check we are not shooting past the memory of the A COO */
        if (a_row_offset + a_itr >= A->NZ)
            break;

        /* check we are still in the correct row, otherwise we are done */
        // (do check here so we are more parallelisable)
        if (A->coords[a_row_offset + a_itr].i != a_row)
            break;

        a_col = A->coords[a_row_offset + a_itr].j;

        /* find row of B that corresponds to this column of A */
        b_row_offset = b_row_offsets[a_col];
        /* if row does not exist, jump to next A column */
        if (b_row_offset == -1)
            continue;

        /* loop will overshoot, break when end of row is reached */
        #pragma vector always
        for (b_itr = 0; b_itr < B->n; b_itr++) {

            /* check we are not shooting past the memory of the B COO */
            if (b_row_offset + b_itr >= B->NZ)
                break;

            b_row = B->coords[b_row_offset + b_itr].i;
            /* check we are still in the correct row of B, otherwise we are done */
            /* (row of b corresponds to the column of A) */
            if (b_row != a_col)
                break;

            /* b_col corresponds to position in output row */
            b_col = B->coords[b_row_offset + b_itr].j;

            /* is there already some existing value for this result position? - if so, this is which index it is at */
            /* this is a pointer to the `struct hash_int` in the hash table (for fast lookup) */
            existing_offset = find_offset_from_hash_table(&pass_start_positions_for_col, b_col);
            // printf("offset for: %d,%d: %p\n",a_row,b_col,existing_offset);
            
            result = A->data[a_row_offset + a_itr] * B->data[b_row_offset + b_itr];

            if (existing_offset != NULL && result != 0.0) {
                // printf("FOUND EXISTING OFFSET: %d,%d: %d\n", a_row, b_col, existing_offset->offset);

                // just add to previous is there is an existing entry
                row->data[existing_offset->offset] += result;
            }
            /* if this is the first value for this column, increment non-zeros! */
            else if (result != 0.0) {
                // printf("NEW VALUE! : %d,%d: %.2f\n", a_row, b_col, result);
                /* row and column in context of the full matrix result */
                /* only need to do this if there is not an existing value */
                row->coords[non_zero_elements].i = a_row;
                row->coords[non_zero_elements].j = b_col;
                row->data[non_zero_elements] = result;
                add_hash_int(&pass_start_positions_for_col, b_col, non_zero_elements);
                non_zero_elements++;
                if (non_zero_elements==current_allocated_size) {
                    current_allocated_size += initial_rows; // add more space for more elements
                    row->coords = (struct coord*)realloc(row->coords,current_allocated_size*sizeof(struct coord));
                    row->data = (double*)realloc(row->data,current_allocated_size*sizeof(double));
                    row->NZ = current_allocated_size; // not really the number of non-zeros, just space allocated at the moment
                }
            }   
        }
    }

    /* deallocate the hash table */
    clear_hash_table(&pass_start_positions_for_col);

    /* if there are no elements in the row, free what we started with and set value to NULL */
    if (non_zero_elements == 0) {
        free_sparse(&row);
        *row_res = NULL;
        return;
    }

    /* cut off the bottom element spaces that were not needed */
    /* and finally fix the number of non-zeros in the result row */
    if (non_zero_elements < current_allocated_size) {
        row->coords = (struct coord*)realloc(row->coords,non_zero_elements*sizeof(struct coord));
        row->data = (double*)realloc(row->data,non_zero_elements*sizeof(double));
        row->NZ = non_zero_elements;
    }


    // printf("ROW %d\n",a_row);
    // print_sparse(row);

    *row_res = row;

}


/* performs sparse multiplication */
/* handles any required sorting, swapping, pre-processing etc. */
/* just pass matricies of __valid sizes__ and let's go! */
static void perform_sparse_optimised_multi( COO A,  COO B, COO *C) {

    const int a_num_rows = A->m;
    const int b_num_cols = B->n;

    #if SHOULD_PROFILE
    LIKWID_MARKER_START("pre-process-multi");
    #endif

    /* if the output will have more columns than rows, calculate via distributed columns */
    /* this will allow for better parellelisation for this matrix */
    /* we do this by transposing both matrices and then swapping them */
    /* then, transpose result back at the end */
    /* this allows us to use the same `row fragmentation` code, but calculating the result a column at a time (instead of a row at a time) */
    /* this is designed to allow flatter matrices to also experience the same awesome parallelisation speedups */
    /* only downside is the time required for swapping and transposing */
    /* fortunately, swap very, very quick and the transpose can be very easily parallelled! */
    char COL_BASED_FRAGS = 0; 
    if (b_num_cols > a_num_rows) {
        // printf("dividing by col!!!\n");
        COL_BASED_FRAGS = 1;
        SWAP(A,B);
        transpose_coo_matrix(&A);
        transpose_coo_matrix(&B);
    }

    order_coo_matrix_rows(A);
    order_coo_matrix_rows(B);

    #if SHOULD_PROFILE
    LIKWID_MARKER_STOP("pre-process-multi");
    #endif

    /* offsets to easily locate row indices */
    int *a_row_offsets = row_offset_table(A);
    int *b_row_offsets = row_offset_table(B);

    int num_fragments = (COL_BASED_FRAGS) ? b_num_cols : a_num_rows;
    COO *to_merge = (COO *)malloc(num_fragments*sizeof(COO));

    COO row;
    int k;
    // THIS is where the ultimate parallelisation payoff comes in!
    #pragma acc kernels
    for (k=0;k<num_fragments;k++) {
        calculate_result_row(k,A,a_row_offsets,B,b_row_offsets,&row);
        to_merge[k] = row;
    }

    /* merge the row results, to get the final matrix C! */
    merge_result_rows(A->m,A->m,B->n,to_merge,C);

    /* if we divided by cols, the result will be the transpose of what we expect */
    if (COL_BASED_FRAGS) {
        transpose_coo_matrix(C);
    }

    /* we no longer need the offset tables */
    free(a_row_offsets);
    free(b_row_offsets);
    free(to_merge);
}



/* Computes C = A*B.
 * C should be allocated by this routine.
 */
void optimised_sparsemm(const COO A, const COO B, COO *C) {
    
    #if SHOULD_PROFILE
    LIKWID_MARKER_INIT;
    #endif

    // m = A rows
    // n = B columns
    // k = A columns
    int m, n, k;

    // ensure there is no value currently stored at C
    *C = NULL;

    // check that the matrices are compatible sizes
    m = A->m;
    k = A->n;
    n = B->n;
    if (k != B->m) {
        fprintf(stderr, "Invalid matrix sizes, got %d x %d and %d x %d\n", A->m, A->n, B->m, B->n);
        exit(1);
    }

    #if SHOULD_PROFILE
    LIKWID_MARKER_START("optimised-multi");
    #endif

    // printf("MULTIPLY OPT START\n");

    /* just multiply! */
    perform_sparse_optimised_multi(A, B, C);

    // printf("MULTIPLY OPT STOP\n");

    #if SHOULD_PROFILE
    LIKWID_MARKER_STOP("optimised-multi");
    LIKWID_MARKER_CLOSE;
    #endif
    
    //return basic_sparsemm(A,B,C);
}



/* merge the matrices A and B
 * merging is performed according to the following critereon:
 * - result be A
 * - entries from B we want to remove should have value zeroed out (otherwise duplicate row/cols will appear)
 * - result will not be ordered
 * (called from add matrices)
 * time complexity: O(n)
 * space complexity: O(n)
 */
static void merge_matrices(COO *A, COO B, int b_uniques) {

    COO added = *A;
    
    // update A values to reflect the merge
    const int old_a_size = added->NZ;
    added->NZ += b_uniques;

    // realloc A so it's large enough to store B's unique entries as well
    added->coords = (struct coord*)realloc(added->coords,added->NZ*sizeof(struct coord));
    added->data = (double*)realloc(added->data,added->NZ*sizeof(double));

    // iterate over B and append all the entries to A
    int k,j;
    j = old_a_size;
    #pragma nounroll
    for (k = 0; k < B->NZ; k++) {
        // do not append if column is 0 - this value has already been added to A
        if (B->data[k] != 0.0) {
            added->data[j] = B->data[k];
            added->coords[j] = B->coords[k];
            j++; // increment A memory location
        }
    }

    *A = added;
    
}

/* add the matrices A and B, storing the result in A.
 * we require messing up entries of B (zeroing some out), so we dealloc B after because it is now useless
 * DO NOT USE B AFTER DEALLOCATED
 * time complexity: O(n)
 * space complexity: O(n)
 */
static void add_matrices(COO *A, COO B) {

    /* de-ref A so we can work on the COO directly */
    COO added = *A;
    
    // the one with more non-zero values should be A
    // this reduces the amount of binary searching and `reallocing` we have to do
    // (since this is only an add operation, order does not matter)
    if (B->NZ > added->NZ) {
        SWAP( added, B);
    }
    
    /* B must be ordered so we can quickly find it's elements! */
    order_coo_matrix_rows(B);
    /* b row offset table for reference */
    int *b_row_offset_table = row_offset_table(B);

    int k;
    int b_non_uniques = 0; // the number of elements of B for which there is a matching element in A
    char not_found_flag = 0;
    double val;
    /* KEY PERFORMANCE IMPROVEMENTS VIA PARALLEL HERE */
    #pragma acc kernels
    /* iterate over each line of A, to see if we can find B vals that correspond */
    /* we know that we iterate over each line of A in order, sorted by ROW, COL, so we can use this to narrow down the bsearch as we go */
    for (k = 0; k < added->NZ; k++) {
        not_found_flag = 0; // reset the flag
        /* find matching entry in B, then add to A */
        /* forceinline so we can vectorise loop */
        val = locate_matching_entry_rows(B,b_row_offset_table,added->coords[k],1,&not_found_flag);
        /* flag will not have been modified if we have found a data val */
        if (not_found_flag == 0) {
            added->data[k] += val;
            b_non_uniques++;
        }
    }

    /* set the return pointer now we have done the calc */
    *A = added;

    /* A now contains all common added values, B contains unique values that should be merged */
    const int unique_b_values = B->NZ-b_non_uniques;
    if (unique_b_values != 0) {
        merge_matrices(A, B, unique_b_values);
    }
    
    // B is now useless
    // (it got messed up bad when we were locating matching add values)
    /* only free the offset table though or the calling function will fail */
    free(b_row_offset_table);
}

/* Computes O = (A + B + C) (D + E + F).
 * O should be allocated by this routine.
 */
void optimised_sparsemm_sum(const COO A, const COO B, const COO C,
                            const COO D, const COO E, const COO F,
                            COO *O) {
//
    #if SHOULD_PROFILE
    LIKWID_MARKER_INIT;
    #endif

    // check that matrices are all compatible sizes
    int i, j, m, n, k;

    m = A->m;
    k = A->n;
    n = D->n;
    if (A->m != B->m || A->n != B->n) {
        fprintf(stderr, "A (%d x %d) and B (%d x %d) are not the same shape\n",
                A->m, A->n, B->m, B->n);
        exit(1);
    }
    if (A->m != C->m || A->n != C->n) {
        fprintf(stderr, "A (%d x %d) and C (%d x %d) are not the same shape\n",
                A->m, A->n, C->m, C->n);
        exit(1);
    }
    if (D->m != E->m || D->n != E->n) {
        fprintf(stderr, "D (%d x %d) and E (%d x %d) are not the same shape\n",
                D->m, D->n, E->m, E->n);
        exit(1);
    }
    if (D->m != F->m || D->n != F->n) {
        fprintf(stderr, "D (%d x %d) and F (%d x %d) are not the same shape\n",
                D->m, D->n, F->m, F->n);
        exit(1);
    }

    if (A->n != D->m) {
        fprintf(stderr, "Invalid matrix sizes, got %d x %d and %d x %d\n",
                A->m, A->n, D->m, D->n);
        exit(1);
    }

    #if SHOULD_PROFILE
    LIKWID_MARKER_START("optimised-sum-add");
    #endif

    /* CREATE MULT MATRIX A */
    COO a_mut = A;
    COO *abc_added = &a_mut;

    // printf("Adding A+B\n");
    // printf("---> A (%d)\n", (*abc_added)->NZ);
    // print_sparse(*abc_added);
    // printf("---> B (%d)\n", B->NZ);
    // print_sparse(B);
    add_matrices(abc_added,B);
    // printf("Adding A+C\n");
    // printf("---> A\n");
    // print_sparse(*abc_added);
    // printf("---> C\n");
    // print_sparse(C);
    add_matrices(abc_added,C);
    // printf("ADDED A,B,C :):\n");
    // print_sparse(*abc_added);

    /* CREATE MULT MATRIX D */
    COO d_mut = D;
    COO *def_added = &d_mut;

    // printf("Adding D+E\n");
    // printf("---> D\n");
    // print_sparse(*def_added);
    // printf("---> E\n");
    // print_sparse(E);
    add_matrices(def_added,E);
    // printf("Adding D+F\n");
    // printf("---> D\n");
    // print_sparse(*def_added);
    // printf("---> F\n");
    // print_sparse(F);
    add_matrices(def_added,F);
    // printf("ADDED D,E,F :):\n");
    // print_sparse(*def_added);

    #if SHOULD_PROFILE
    LIKWID_MARKER_STOP("optimised-sum-add");
    #endif

    

    // ensure there is no value currently stored at O
    *O = NULL;

    #if SHOULD_PROFILE
    LIKWID_MARKER_START("optimised-sum-multiply");
    #endif

    // perform the optimised matrix multiplication operation
    // printf("multiplying A*D...\n");
    perform_sparse_optimised_multi(*abc_added, *def_added, O);
    // printf("mult done!\n");

    #if SHOULD_PROFILE
    LIKWID_MARKER_STOP("optimised-sum-multiply");
    LIKWID_MARKER_CLOSE;
    #endif

//    return basic_sparsemm_sum(A, B, C, D, E, F, O);
}
