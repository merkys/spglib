/* Copyright (C) 2008 Atsushi Togo */
/* All rights reserved. */

/* This file is part of spglib. */

/* Redistribution and use in source and binary forms, with or without */
/* modification, are permitted provided that the following conditions */
/* are met: */

/* * Redistributions of source code must retain the above copyright */
/*   notice, this list of conditions and the following disclaimer. */

/* * Redistributions in binary form must reproduce the above copyright */
/*   notice, this list of conditions and the following disclaimer in */
/*   the documentation and/or other materials provided with the */
/*   distribution. */

/* * Neither the name of the phonopy project nor the names of its */
/*   contributors may be used to endorse or promote products derived */
/*   from this software without specific prior written permission. */

/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS */
/* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT */
/* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS */
/* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE */
/* COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, */
/* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, */
/* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; */
/* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER */
/* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT */
/* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN */
/* ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE. */

#include <stdio.h>
#include <stdlib.h>
#include "cell.h"
#include "delaunay.h"
#include "mathfunc.h"
#include "primitive.h"
#include "symmetry.h"

#include "debug.h"

#define REDUCE_RATE 0.95
#define NUM_ATTEMPT 20

static Primitive * get_primitive(const Cell * cell,
				 const double symprec,
				 const double angle_tolerance);
static Cell * get_cell_with_smallest_lattice(const Cell * cell,
                                             const double symprec);
static Cell * get_primitive_cell(int * mapping_table,
                                 const Cell * cell,
                                 const VecDBL * pure_trans,
                                 const double symprec,
                                 const double angle_tolerance);
static int get_primitive_lattice_vectors_iterative
(double prim_lattice[3][3],
 const Cell * cell,
 const VecDBL * pure_trans,
 const double symprec,
 const double angle_tolerance);
static int get_primitive_lattice_vectors(double prim_lattice[3][3],
                                         const VecDBL * vectors,
                                         const Cell * cell,
                                         const double symprec);
static VecDBL * get_translation_candidates(const VecDBL * pure_trans);

/* return NULL if failed */
Primitive * prm_alloc_primitive(const int size)
{
  Primitive *primitive;
  int i, j;

  primitive = NULL;

  if ((primitive = (Primitive*) malloc(sizeof(Primitive))) == NULL) {
    warning_print("spglib: Memory could not be allocated ");
    return NULL;
  }

  primitive->cell = NULL;
  primitive->mapping_table = NULL;
  primitive->size = size;
  primitive->tolerance = 0;
  primitive->angle_tolerance = -1.0;

  if ((primitive->t_mat = (double (*)[3]) malloc(sizeof(double[3]) * 3)) ==
      NULL) {
    warning_print("spglib: Memory could not be allocated.");
    free(primitive);
    primitive = NULL;
    return NULL;
  }

  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      primitive->t_mat[i][j] = 0;
    }
  }

  if (size > 0) {
    if ((primitive->mapping_table = (int*) malloc(sizeof(int) * size)) == NULL) {
      warning_print("spglib: Memory could not be allocated ");
      warning_print("(Primitive, line %d, %s).\n", __LINE__, __FILE__);
      free(primitive->t_mat);
      primitive->t_mat = NULL;
      free(primitive);
      primitive = NULL;
      return NULL;
    }
  }

  for (i = 0; i < size; i++) {
    primitive->mapping_table[i] = -1;
  }

  return primitive;
}

void prm_free_primitive(Primitive * primitive)
{
  if (primitive != NULL) {
    if (primitive->mapping_table != NULL) {
      free(primitive->mapping_table);
      primitive->mapping_table = NULL;
    }

    if (primitive->t_mat != NULL) {
      free(primitive->t_mat);
      primitive->t_mat = NULL;
    }

    if (primitive->cell != NULL) {
      cel_free_cell(primitive->cell);
      primitive->cell = NULL;
    }
    free(primitive);
  }
}

/* Return NULL if failed */
Primitive * prm_get_primitive(const Cell * cell,
			      const double symprec,
			      const double angle_tolerance)
{
  return get_primitive(cell, symprec, angle_tolerance);
}

/* Return NULL if failed */
static Primitive * get_primitive(const Cell * cell,
				 const double symprec,
				 const double angle_tolerance)
{
  int i, attempt;
  double tolerance;
  double inv_lat[3][3];
  Primitive *primitive;
  VecDBL * pure_trans;

  debug_print("get_primitive (tolerance = %f):\n", symprec);

  primitive = NULL;
  pure_trans = NULL;

  if ((primitive = prm_alloc_primitive(cell->size)) == NULL) {
    goto notfound;
  }

  tolerance = symprec;
  for (attempt = 0; attempt < NUM_ATTEMPT; attempt++) {
    debug_print("get_primitive (attempt = %d):\n", attempt);
    if ((pure_trans = sym_get_pure_translation(cell, tolerance)) == NULL) {
      goto cont;
    }

    if (pure_trans->size == 1) {
      if ((primitive->cell = get_cell_with_smallest_lattice(cell, tolerance))
          != NULL) {
        for (i = 0; i < cell->size; i++) {
          primitive->mapping_table[i] = i;
        }
        goto found;
      }
    } else {
      if ((primitive->cell = get_primitive_cell(primitive->mapping_table,
                                                cell,
                                                pure_trans,
                                                tolerance,
						angle_tolerance)) != NULL) {
        goto found;
      }
    }

    mat_free_VecDBL(pure_trans);
    pure_trans = NULL;

  cont:
    tolerance *= REDUCE_RATE;
    warning_print("spglib: Reduce tolerance to %f ", tolerance);
    warning_print("(line %d, %s).\n", __LINE__, __FILE__);
  }

  prm_free_primitive(primitive);
  primitive = NULL;

 notfound:
  return NULL;

 found:
  primitive->tolerance = tolerance;
  primitive->angle_tolerance = angle_tolerance;
  mat_inverse_matrix_d3(inv_lat, cell->lattice, 0);
  mat_multiply_matrix_d3(primitive->t_mat, primitive->cell->lattice, inv_lat);
  mat_free_VecDBL(pure_trans);
  pure_trans = NULL;
  return primitive;
}

/* Return NULL if failed */
static Cell * get_cell_with_smallest_lattice(const Cell * cell,
                                             const double symprec)
{
  int i, j;
  double min_lat[3][3], trans_mat[3][3], inv_lat[3][3];
  Cell * smallest_cell;

  debug_print("get_cell_with_smallest_lattice:\n");

  smallest_cell = NULL;

  if (!del_delaunay_reduce(min_lat, cell->lattice, symprec)) {
    return NULL;
  }

  mat_inverse_matrix_d3(inv_lat, min_lat, 0);
  mat_multiply_matrix_d3(trans_mat, inv_lat, cell->lattice);

  if ((smallest_cell = cel_alloc_cell(cell->size)) == NULL) {
    return NULL;
  }

  mat_copy_matrix_d3(smallest_cell->lattice, min_lat);
  for (i = 0; i < cell->size; i++) {
    smallest_cell->types[i] = cell->types[i];
    mat_multiply_matrix_vector_d3(smallest_cell->position[i],
                                  trans_mat, cell->position[i]);
    for (j = 0; j < 3; j++) {
      smallest_cell->position[i][j] = mat_Dmod1(smallest_cell->position[i][j]);
    }
  }

  return smallest_cell;
}

/* Return NULL if failed */
static Cell * get_primitive_cell(int * mapping_table,
                                 const Cell * cell,
                                 const VecDBL * pure_trans,
                                 const double symprec,
                                 const double angle_tolerance)
{
  int multi;
  double prim_lat[3][3], smallest_lat[3][3];
  Cell * primitive_cell;

  debug_print("get_primitive_cell:\n");

  primitive_cell = NULL;

  /* Primitive lattice vectors are searched. */
  /* To be consistent, sometimes tolerance is decreased iteratively. */
  /* The descreased tolerance is stored in 'static double tolerance'. */
  multi = get_primitive_lattice_vectors_iterative(prim_lat,
                                                  cell,
                                                  pure_trans,
                                                  symprec,
						  angle_tolerance);
  if (! multi) {
    goto not_found;
  }

  if (! del_delaunay_reduce(smallest_lat, prim_lat, symprec)) {
    goto not_found;
  }

  /* Fit atoms into new primitive cell */
  if ((primitive_cell = cel_trim_cell(mapping_table,
                                      smallest_lat,
                                      cell,
                                      symprec)) == NULL) {
    goto not_found;
  }

  /* found */
  return primitive_cell;

 not_found:
  warning_print("spglib: Primitive cell could not be found ");
  warning_print("(line %d, %s).\n", __LINE__, __FILE__);
  return NULL;
}


/* Return 0 if failed */
static int get_primitive_lattice_vectors_iterative(double prim_lattice[3][3],
                                                   const Cell * cell,
                                                   const VecDBL * pure_trans,
                                                   const double symprec,
                                                   const double angle_tolerance)
{
  int i, multi, attempt;
  double tolerance;
  VecDBL * vectors, * pure_trans_reduced, *tmp_vec;

  vectors = NULL;
  pure_trans_reduced = NULL;
  tmp_vec = NULL;

  tolerance = symprec;

  if ((pure_trans_reduced = mat_alloc_VecDBL(pure_trans->size)) == NULL) {
    goto fail;
  }

  for (i = 0; i < pure_trans->size; i++) {
    mat_copy_vector_d3(pure_trans_reduced->vec[i], pure_trans->vec[i]);
  }

  for (attempt = 0; attempt < NUM_ATTEMPT; attempt++) {
    multi = pure_trans_reduced->size;

    if ((vectors = get_translation_candidates(pure_trans_reduced)) == NULL) {
      mat_free_VecDBL(pure_trans_reduced);
      pure_trans_reduced = NULL;
      goto fail;
    }

    /* Lattice of primitive cell is found among pure translation vectors */
    if (get_primitive_lattice_vectors(prim_lattice,
                                      vectors,
                                      cell,
                                      tolerance)) {

      mat_free_VecDBL(vectors);
      vectors = NULL;
      mat_free_VecDBL(pure_trans_reduced);
      pure_trans_reduced = NULL;

      goto found;

    } else {

      if ((tmp_vec = mat_alloc_VecDBL(multi)) == NULL) {
        mat_free_VecDBL(vectors);
        vectors = NULL;
        mat_free_VecDBL(pure_trans_reduced);
        pure_trans_reduced = NULL;
        goto fail;
      }

      for (i = 0; i < multi; i++) {
        mat_copy_vector_d3(tmp_vec->vec[i], pure_trans_reduced->vec[i]);
      }
      mat_free_VecDBL(pure_trans_reduced);
      pure_trans_reduced = NULL;

      pure_trans_reduced = sym_reduce_pure_translation(cell,
                                                       tmp_vec,
                                                       tolerance,
						       angle_tolerance);

      mat_free_VecDBL(tmp_vec);
      tmp_vec = NULL;
      mat_free_VecDBL(vectors);
      vectors = NULL;

      if (pure_trans_reduced == NULL) {
        goto fail;
      }

      warning_print("spglib: Tolerance is reduced to %f (%d), ",
                    tolerance, attempt);
      warning_print("num_pure_trans = %d\n", pure_trans_reduced->size);

      tolerance *= REDUCE_RATE;
    }
  }

  mat_free_VecDBL(pure_trans_reduced);
  pure_trans_reduced = NULL;

 fail:
  return 0;

 found:
  return multi;
}

/* Return 0 if failed */
static int get_primitive_lattice_vectors(double prim_lattice[3][3],
                                         const VecDBL * vectors,
                                         const Cell * cell,
                                         const double symprec)
{
  int i, j, k, size;
  double initial_volume, volume;
  double relative_lattice[3][3], min_vectors[3][3], tmp_lattice[3][3];
  double inv_mat_dbl[3][3];
  int inv_mat_int[3][3];

  debug_print("get_primitive_lattice_vectors:\n");

  size = vectors->size;
  initial_volume = mat_Dabs(mat_get_determinant_d3(cell->lattice));

  /* check volumes of all possible lattices, find smallest volume */
  for (i = 0; i < size; i++) {
    for (j = i + 1; j < size; j++) {
      for (k = j + 1; k < size; k++) {
        mat_multiply_matrix_vector_d3(tmp_lattice[0],
                                      cell->lattice,
                                      vectors->vec[i]);
        mat_multiply_matrix_vector_d3(tmp_lattice[1],
                                      cell->lattice,
                                      vectors->vec[j]);
        mat_multiply_matrix_vector_d3(tmp_lattice[2],
                                      cell->lattice,
                                      vectors->vec[k]);
        volume = mat_Dabs(mat_get_determinant_d3(tmp_lattice));
        if (volume > symprec) {
          if (mat_Nint(initial_volume / volume) == size-2) {
            mat_copy_vector_d3(min_vectors[0], vectors->vec[i]);
            mat_copy_vector_d3(min_vectors[1], vectors->vec[j]);
            mat_copy_vector_d3(min_vectors[2], vectors->vec[k]);
            goto ret;
          }
        }
      }
    }
  }

  /* Not found */
  warning_print("spglib: Primitive lattice vectors cound not be found ");
  warning_print("(line %d, %s).\n", __LINE__, __FILE__);
  return 0;

  /* Found */
 ret:
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      relative_lattice[j][i] = min_vectors[i][j];
    }
  }

  mat_inverse_matrix_d3(inv_mat_dbl, relative_lattice, 0);
  mat_cast_matrix_3d_to_3i(inv_mat_int, inv_mat_dbl);
  if (abs(mat_get_determinant_i3(inv_mat_int)) == size-2) {
    mat_cast_matrix_3i_to_3d(inv_mat_dbl, inv_mat_int);
    mat_inverse_matrix_d3(relative_lattice, inv_mat_dbl, 0);
  } else {
    warning_print("spglib: Primitive lattice cleaning is incomplete ");
    warning_print("(line %d, %s).\n", __LINE__, __FILE__);
  }
  mat_multiply_matrix_d3(prim_lattice, cell->lattice, relative_lattice);

  return 1;
}

static VecDBL * get_translation_candidates(const VecDBL * pure_trans)
{
  int i, j, multi;
  VecDBL * vectors;

  vectors = NULL;
  multi = pure_trans->size;

  if ((vectors = mat_alloc_VecDBL(multi + 2)) == NULL) {
    return NULL;
  }

  /* store pure translations in original cell */
  /* as trial primitive lattice vectors */
  for (i = 0; i < multi - 1; i++) {
    mat_copy_vector_d3(vectors->vec[i], pure_trans->vec[i + 1]);
  }

  /* store lattice translations of original cell */
  /* as trial primitive lattice vectors */
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      if (i == j) {
        vectors->vec[i+multi-1][j] = 1;
      } else {
        vectors->vec[i+multi-1][j] = 0;
      }
    }
  }

  return vectors;
}
