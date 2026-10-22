/* Exact orientation predicates: a trimmed copy of Jonathan Richard Shewchuk's
 * public-domain predicates.c (see third_party/README.md). Call
 * three_d_exactinit() once before either predicate. */
#ifndef THREE_D_PREDICATES_H
#define THREE_D_PREDICATES_H

#ifdef __cplusplus
extern "C" {
#endif

void three_d_exactinit(void);

/* Positive when pa, pb, pc occur in counterclockwise order, negative when
 * clockwise, zero when collinear; the sign is exact. */
double three_d_orient2d(const double *pa, const double *pb, const double *pc);

/* Positive when pd lies below the plane through pa, pb, pc (which appear
 * counterclockwise seen from above), negative above, zero when coplanar; the
 * sign is exact. */
double three_d_orient3d(const double *pa, const double *pb, const double *pc, const double *pd);

#ifdef __cplusplus
}
#endif

#endif
