# pip install shapely ezdxf numpy
import ezdxf
import numpy as np
import shapely
from shapely.geometry import Point, box
from shapely.ops import unary_union

W, H = 150, 100  # plate (mm), origin at bottom left
MARGIN = 5  # solid border
RIB = 2.0  # rib width (≈ multiple of the extrusion width)
FILLET = 2.0  # cell corner radius
N_SEEDS = 40
MIN_AREA = 6  # drop cells that are too small (mm²)
holes = []  # [(10, 10, 1.7), (140, 10, 1.7), (10, 90, 1.7), (140, 90, 1.7)]  # (x,y,r)
keep_rects = [(0, 0, 150, 25)]  # solid areas (support feet): x0,y0,x1,y1
KEEP_PAD = 3  # material around holes/supports

rng = np.random.default_rng(1)
inner = box(MARGIN, MARGIN, W - MARGIN, H - MARGIN)
keep = unary_union(
    [Point(x, y).buffer(r + KEEP_PAD) for x, y, r in holes]
    + [box(*r).buffer(KEEP_PAD) for r in keep_rects]
)
region = inner.difference(keep)  # area that can be hollowed out

pts = rng.uniform([0, 0], [W, H], (N_SEEDS, 2))
for _ in range(10):  # Lloyd relaxation → regular cells
    cells = shapely.voronoi_polygons(
        shapely.MultiPoint(pts), extend_to=box(-W, -H, 2 * W, 2 * H)
    ).geoms
    pts = np.array([c.intersection(box(0, 0, W, H)).centroid.coords[0] for c in cells])

cells = shapely.voronoi_polygons(
    shapely.MultiPoint(pts), extend_to=box(-W, -H, 2 * W, 2 * H)
).geoms
doc = ezdxf.new(units=4)
msp = doc.modelspace()  # units=4 → mm
for c in cells:
    p = c.intersection(region).buffer(-RIB / 2, join_style="mitre")
    p = p.buffer(-FILLET).buffer(FILLET)
    for g in getattr(p, "geoms", [p]):
        if g.is_empty or g.area < MIN_AREA:
            continue
        msp.add_lwpolyline(list(g.exterior.coords), close=True)

doc.saveas("voronoi.dxf")
