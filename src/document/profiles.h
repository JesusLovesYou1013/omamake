#pragma once

#include <vector>

// The profile finder (owner, 2026-10-30): which areas a sketch's lines, arcs and curves close
// off, as Fusion's profiles (two overlapping circles: three profiles; a circle inside a
// rectangle: the ring and the disc), and where lines nearly meet but don't (Find Profile Gaps).
// Works on plain point chains in the sketch's own flat coordinates (mm), so it knows nothing
// of the engine or the solver.
namespace profiles {

struct P {
    double u = 0, v = 0;
};
using Path = std::vector<P>;

// A closed area: its outline (anticlockwise) and the islands cut out of it (each its own
// profile too). area: what's left, in mm².
struct Profile {
    Path outer;
    std::vector<Path> holes;
    double area = 0;
};

// Where two lines come within the gap distance of each other without touching, and aren't
// joined to each other nearby either: the nearest point of each (a, b).
struct Gap {
    P a, b;
    double size = 0;
};

struct Result {
    std::vector<Profile> profiles;
    std::vector<Gap> gaps;
};

// paths: every normal (not construction, not helper) line, arc and curve as a chain of
// points. Closer than `touching` counts as joined; gaps are up to `gapMax` across.
// groups (one per path, or empty): paths sharing a group above 0 are never a gap to each
// other (one piece of text: its letters' own narrow strokes and spacing aren't for fixing).
// gapMax 0: profiles only, no gaps looked for (quicker, for the shading).
Result find(const std::vector<Path> &paths, double gapMax = 1.0, double touching = 1e-4,
            const std::vector<int> &groups = {});

// A profile cut into triangles (its holes left out), as indices into its points: the outline's
// first, then each hole's in turn. Empty if it can't be (a self-crossing outline).
std::vector<int> triangles(const Profile &profile);

// Is p inside the profile (in its outline, not in a hole)?
bool contains(const Profile &profile, P p);

} // namespace profiles
