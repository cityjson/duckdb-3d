#include "kernel/convex_hull.hpp"

#include "kernel/predicates.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb_3d {

namespace {

bool LexLess(const Vertex3D &a, const Vertex3D &b) {
	if (a.x != b.x) {
		return a.x < b.x;
	}
	if (a.y != b.y) {
		return a.y < b.y;
	}
	return a.z < b.z;
}

double Distance2(const Vertex3D &a, const Vertex3D &b) {
	const double dx = a.x - b.x;
	const double dy = a.y - b.y;
	const double dz = a.z - b.z;
	return dx * dx + dy * dy + dz * dz;
}

//! |(b - a) x (c - a)|^2, in doubles: only used to prefer well-shaped seeds;
//! every decision is taken by the exact predicates.
double Area2(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c) {
	const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
	const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
	const double nx = uy * vz - uz * vy;
	const double ny = uz * vx - ux * vz;
	const double nz = ux * vy - uy * vx;
	return nx * nx + ny * ny + nz * nz;
}

double Volume6(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c, const Vertex3D &d) {
	const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
	const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
	const double wx = d.x - a.x, wy = d.y - a.y, wz = d.z - a.z;
	return ux * (vy * wz - vz * wy) - uy * (vx * wz - vz * wx) + uz * (vx * wy - vy * wx);
}

GeomModel PointModel(const std::vector<Vertex3D> &vertices) {
	GeomModel out;
	out.type = GeomType::Point;
	out.vertices = vertices;
	out.ComputeBBox();
	return out;
}

//! Hull of exactly coplanar points: Andrew's monotone chain in the coordinate
//! plane `axis` drops, where the plane does not degenerate. Projection along an
//! axis the plane is not parallel to is an affine bijection of the plane, so
//! the exact 2D orientations are the 3D ones up to one global sign.
GeomModel PlanarHull(const std::vector<Vertex3D> &pts, int axis) {
	const int u = (axis + 1) % 3;
	const int v = (axis + 2) % 3;
	auto coord = [](const Vertex3D &p, int i) {
		return i == 0 ? p.x : (i == 1 ? p.y : p.z);
	};
	std::vector<Vertex3D> sorted = pts;
	std::sort(sorted.begin(), sorted.end(), [&](const Vertex3D &a, const Vertex3D &b) {
		if (coord(a, u) != coord(b, u)) {
			return coord(a, u) < coord(b, u);
		}
		return coord(a, v) < coord(b, v);
	});
	// Distinct 3D points on one plane project to distinct 2D points.
	std::vector<Vertex3D> ring;
	ring.reserve(sorted.size() + 1);
	for (int pass = 0; pass < 2; pass++) {
		const size_t floor = ring.size();
		for (size_t n = 0; n < sorted.size(); n++) {
			const Vertex3D &p = pass == 0 ? sorted[n] : sorted[sorted.size() - 1 - n];
			while (ring.size() >= floor + 2 && Orient2D(ring[ring.size() - 2], ring.back(), p, axis) <= 0) {
				ring.pop_back();
			}
			ring.push_back(p);
		}
		ring.pop_back(); // each chain's last point starts the other chain
	}
	GeomModel out;
	out.type = GeomType::Polygon;
	out.vertices = std::move(ring);
	out.ring_offsets = {0, static_cast<uint32_t>(out.vertices.size())};
	out.ComputeBBox();
	return out;
}

//! Incremental 3D hull with conflict lists (Clarkson-Shor, in quickhull's
//! farthest-point order). Faces are triangles wound counterclockwise seen from
//! outside; a point is outside a face exactly when Orient3D says it is strictly
//! on the face's normal side. Coplanar points are never "outside", so faces
//! that end up sharing a plane are merged afterwards.
class IncrementalHull {
public:
	explicit IncrementalHull(const std::vector<Vertex3D> &pts) : pts_(pts) {
	}

	void Build(const std::array<uint32_t, 4> &seed) {
		const uint32_t a = seed[0], b = seed[1], c = seed[2], d = seed[3];
		std::array<std::array<uint32_t, 3>, 4> tetra = {{{a, b, c}, {a, d, b}, {b, d, c}, {c, d, a}}};
		if (Orient3D(pts_[a], pts_[b], pts_[c], pts_[d]) > 0) {
			for (auto &f : tetra) {
				std::swap(f[1], f[2]);
			}
		}
		std::vector<uint32_t> fresh;
		fresh.reserve(tetra.size());
		for (const auto &f : tetra) {
			fresh.push_back(AddFace(f[0], f[1], f[2]));
		}
		std::vector<uint32_t> rest;
		for (uint32_t i = 0; i < pts_.size(); i++) {
			if (i != a && i != b && i != c && i != d) {
				rest.push_back(i);
			}
		}
		Assign(rest, fresh);
		for (uint32_t f = 0; f < faces_.size(); f++) {
			while (faces_[f].alive && !faces_[f].outside.empty()) {
				AddPoint(f);
			}
		}
	}

	GeomModel Surface() const;

private:
	struct Face {
		std::array<uint32_t, 3> v;
		bool alive;
		std::vector<uint32_t> outside;
	};

	static uint64_t EdgeKey(uint32_t from, uint32_t to) {
		return (static_cast<uint64_t>(from) << 32) | to;
	}

	bool Outside(uint32_t face, uint32_t p) const {
		const auto &v = faces_[face].v;
		return Orient3D(pts_[v[0]], pts_[v[1]], pts_[v[2]], pts_[p]) > 0;
	}

	uint32_t AddFace(uint32_t a, uint32_t b, uint32_t c) {
		const auto id = static_cast<uint32_t>(faces_.size());
		faces_.push_back({{a, b, c}, true, {}});
		edge_face_[EdgeKey(a, b)] = id;
		edge_face_[EdgeKey(b, c)] = id;
		edge_face_[EdgeKey(c, a)] = id;
		return id;
	}

	//! Give each point to the first candidate face it is outside of; a point
	//! outside none of them is inside the hull and is dropped for good.
	void Assign(const std::vector<uint32_t> &points, const std::vector<uint32_t> &candidates) {
		for (uint32_t p : points) {
			for (uint32_t f : candidates) {
				if (Outside(f, p)) {
					faces_[f].outside.push_back(p);
					break;
				}
			}
		}
	}

	//! Add the point of `face`'s conflict list farthest from its plane.
	void AddPoint(uint32_t face) {
		const auto &fv = faces_[face].v;
		const Vertex3D &a = pts_[fv[0]], &b = pts_[fv[1]], &c = pts_[fv[2]];
		uint32_t eye = faces_[face].outside[0];
		double best = -1.0;
		for (uint32_t p : faces_[face].outside) {
			const double h = Volume6(a, b, c, pts_[p]);
			if (h > best) {
				best = h;
				eye = p;
			}
		}
		// The faces the eye sees form a connected patch around `face`; its
		// boundary, as directed edges of visible faces whose twin face is
		// hidden, is the horizon.
		std::vector<uint32_t> visible = {face};
		std::vector<char> seen(faces_.size(), 0);
		seen[face] = 1;
		std::vector<std::pair<uint32_t, uint32_t>> horizon;
		for (size_t i = 0; i < visible.size(); i++) {
			const auto v = faces_[visible[i]].v;
			for (int e = 0; e < 3; e++) {
				const uint32_t from = v[e], to = v[(e + 1) % 3];
				const uint32_t twin = edge_face_.at(EdgeKey(to, from));
				if (seen[twin] == 1) {
					continue;
				}
				if (seen[twin] == 0 && Outside(twin, eye)) {
					seen[twin] = 1;
					visible.push_back(twin);
				} else {
					seen[twin] = 2;
					horizon.emplace_back(from, to);
				}
			}
		}
		// Visibility belongs to the face, not to the edge it was reached through,
		// so a face once judged hidden stays hidden and each of its edges to a
		// visible face is recorded once.
		std::vector<uint32_t> orphans;
		for (uint32_t f : visible) {
			auto &dead = faces_[f];
			dead.alive = false;
			for (uint32_t p : dead.outside) {
				if (p != eye) {
					orphans.push_back(p);
				}
			}
			dead.outside.clear();
			dead.outside.shrink_to_fit();
			for (int e = 0; e < 3; e++) {
				edge_face_.erase(EdgeKey(dead.v[e], dead.v[(e + 1) % 3]));
			}
		}
		std::vector<uint32_t> fresh;
		fresh.reserve(horizon.size());
		for (const auto &edge : horizon) {
			fresh.push_back(AddFace(edge.first, edge.second, eye));
		}
		Assign(orphans, fresh);
	}

	const std::vector<Vertex3D> &pts_;
	std::vector<Face> faces_;
	std::unordered_map<uint64_t, uint32_t> edge_face_;
};

GeomModel IncrementalHull::Surface() const {
	// Merge faces that share a plane: union across every edge whose two faces
	// are coplanar, which the exact test decides on the far vertex.
	std::vector<uint32_t> group(faces_.size());
	std::iota(group.begin(), group.end(), 0U);
	auto find = [&group](uint32_t f) {
		while (group[f] != f) {
			group[f] = group[group[f]];
			f = group[f];
		}
		return f;
	};
	for (uint32_t f = 0; f < faces_.size(); f++) {
		if (!faces_[f].alive) {
			continue;
		}
		const auto &v = faces_[f].v;
		for (int e = 0; e < 3; e++) {
			const uint32_t twin = edge_face_.at(EdgeKey(v[(e + 1) % 3], v[e]));
			const auto &w = faces_[twin].v;
			uint32_t far = w[0];
			for (uint32_t x : w) {
				if (x != v[e] && x != v[(e + 1) % 3]) {
					far = x;
				}
			}
			if (Orient3D(pts_[v[0]], pts_[v[1]], pts_[v[2]], pts_[far]) == 0) {
				group[find(f)] = find(twin);
			}
		}
	}
	// Each group's boundary is the directed edges whose twin lies in another
	// group; for a convex polygon it is a single cycle, kept in face order.
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> next_by_group;
	std::vector<uint32_t> order;
	for (uint32_t f = 0; f < faces_.size(); f++) {
		if (!faces_[f].alive) {
			continue;
		}
		const uint32_t g = find(f);
		const auto &v = faces_[f].v;
		for (int e = 0; e < 3; e++) {
			const uint32_t from = v[e], to = v[(e + 1) % 3];
			if (find(edge_face_.at(EdgeKey(to, from))) != g) {
				if (next_by_group.find(g) == next_by_group.end()) {
					order.push_back(g);
				}
				next_by_group[g][from] = to;
			}
		}
	}
	GeomModel out;
	out.type = GeomType::PolyhedralSurface;
	out.part_offsets.push_back(0);
	out.ring_offsets.push_back(0);
	for (uint32_t g : order) {
		const auto &next = next_by_group.at(g);
		std::vector<uint32_t> ring;
		// Start from the smallest index so the output does not depend on hashing.
		uint32_t start = next.begin()->first;
		for (const auto &kv : next) {
			start = std::min(start, kv.first);
		}
		uint32_t at = start;
		do {
			ring.push_back(at);
			at = next.at(at);
		} while (at != start && ring.size() <= next.size());
		// Drop vertices that lie on the segment between their neighbours: points
		// that entered the hull before a later one put them inside an edge.
		bool removed = true;
		while (removed && ring.size() > 3) {
			removed = false;
			for (size_t i = 0; i < ring.size() && ring.size() > 3; i++) {
				const Vertex3D &prev = pts_[ring[(i + ring.size() - 1) % ring.size()]];
				const Vertex3D &cur = pts_[ring[i]];
				const Vertex3D &nxt = pts_[ring[(i + 1) % ring.size()]];
				if (Collinear(prev, cur, nxt)) {
					ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
					removed = true;
					break;
				}
			}
		}
		for (uint32_t idx : ring) {
			out.vertices.push_back(pts_[idx]);
		}
		out.ring_offsets.push_back(static_cast<uint32_t>(out.vertices.size()));
		out.part_offsets.push_back(static_cast<uint32_t>(out.ring_offsets.size() - 1));
	}
	out.ComputeBBox();
	return out;
}

} // namespace

GeomModel ConvexHull3D(const std::vector<Vertex3D> &points) {
	std::vector<Vertex3D> pts = points;
	std::sort(pts.begin(), pts.end(), LexLess);
	pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
	if (pts.size() <= 1) {
		return PointModel(pts);
	}
	// Seeds: the lexicographic minimum, the point farthest from it, the point
	// that spans the largest triangle with them among those not collinear, and
	// the point that spans the largest tetrahedron among those not coplanar.
	// The doubles only rank candidates; exact predicates decide eligibility.
	const uint32_t s0 = 0;
	uint32_t s1 = 1;
	for (uint32_t i = 1; i < pts.size(); i++) {
		if (Distance2(pts[s0], pts[i]) > Distance2(pts[s0], pts[s1])) {
			s1 = i;
		}
	}
	int64_t s2 = -1;
	double best_area = -1.0;
	for (uint32_t i = 1; i < pts.size(); i++) {
		if (i != s1 && !Collinear(pts[s0], pts[s1], pts[i])) {
			const double area = Area2(pts[s0], pts[s1], pts[i]);
			if (area > best_area) {
				best_area = area;
				s2 = i;
			}
		}
	}
	if (s2 < 0) {
		// Collinear: on a line, lexicographic order is the order along it.
		GeomModel line;
		line.type = GeomType::LineString;
		line.vertices = {pts.front(), pts.back()};
		line.ComputeBBox();
		return line;
	}
	const auto t2 = static_cast<uint32_t>(s2);
	int64_t s3 = -1;
	double best_volume = -1.0;
	for (uint32_t i = 1; i < pts.size(); i++) {
		if (Orient3D(pts[s0], pts[s1], pts[t2], pts[i]) != 0) {
			const double volume = std::fabs(Volume6(pts[s0], pts[s1], pts[t2], pts[i]));
			if (volume > best_volume) {
				best_volume = volume;
				s3 = i;
			}
		}
	}
	if (s3 < 0) {
		// Project along z when the plane is not vertical, so the ring turns
		// counterclockwise seen from above; a vertical plane along y, then x.
		int axis = 2;
		while (Orient2D(pts[s0], pts[s1], pts[t2], axis) == 0) {
			axis--;
		}
		return PlanarHull(pts, axis);
	}
	IncrementalHull hull(pts);
	hull.Build({s0, s1, t2, static_cast<uint32_t>(s3)});
	return hull.Surface();
}

} // namespace duckdb_3d
