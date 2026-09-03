/* ui/raster_edge.h: triangle-edge math shared by this directory's two
 * software rasterisers -- ui/title_logo.cpp's binary-coverage fill and
 * ui/ps2_icon_render.cpp's Gouraud-shaded, z-tested fill.
 *
 * Both implement the GS's own fill rule: a pixel belongs to the triangle
 * whose interior contains its centre, and a centre exactly on a shared edge
 * belongs to whichever triangle has that edge as a top or a left one (the
 * two triangles sharing an edge see it in opposite directions, so exactly
 * one of them claims such a centre -- no seam, no double coverage).
 */
#ifndef ICORECOMP_UI_RASTER_EDGE_H
#define ICORECOMP_UI_RASTER_EDGE_H

/* Twice the signed area of the triangle (a, b, c), positive for a clockwise
 * winding in the y-down space both rasterisers work in. Doubles because the
 * inputs are pixel coordinates that can reach a few thousand and the sign
 * has to be exact for the fill rule below. */
inline double rt_orient2d(double ax, double ay, double bx, double by, double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/* For the positive winding orient2d gives, an edge (a, b) is top when it is
 * horizontal with b to the right of a, and left when it runs upward
 * (b.y < a.y). */
inline bool rt_top_left(double ax, double ay, double bx, double by) {
    return (ay == by && bx > ax) || (by < ay);
}

#endif /* ICORECOMP_UI_RASTER_EDGE_H */
