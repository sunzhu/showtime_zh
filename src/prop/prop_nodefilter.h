/*
 *  Property node filters
 *  Copyright (C) 2010 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PROP_NODEFILTER_H__
#define PROP_NODEFILTER_H__

#include "prop.h"

struct prop_nf_pred;
struct prop_nf;

typedef enum {
  PROP_NF_CMP_EQ,
  PROP_NF_CMP_NEQ,
} prop_nf_cmp_t;

typedef enum {
  PROP_NF_MODE_INCLUDE,
  PROP_NF_MODE_EXCLUDE,
} prop_nf_mode_t;

void prop_nf_pred_str_add(struct prop_nf *nf,
			  const char *path, prop_nf_cmp_t cf,
			  const char *str, prop_t *enable,
			  prop_nf_mode_t mode);

void prop_nf_pred_int_add(struct prop_nf *nf,
			  const char *path, prop_nf_cmp_t cf,
			  int value, prop_t *enable,
			  prop_nf_mode_t mode);

struct prop_nf *prop_nf_create(prop_t *dst, prop_t *src,
			       prop_t *filter, const char *defsortpath);

void prop_nf_release(struct prop_nf *nf);

#endif // PROP_NODEFILTER_H__