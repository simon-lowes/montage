// Montage — particles: the "particles" generator, an emitter of sparks, dust, snow, rain, smoke puffs or confetti.
//
// Particles are born from an emitter (a point, line, box, sphere, a title's letters or a picture's alpha or
// brightness) at a birth rate and in a burst at the start, each with its life, speed, direction and spread. Gravity,
// wind, drag, an attractor (a repeller when its strength is negative) and curl-noise turbulence (a divergence-free
// field: smooth, with nowhere for particles to pile up) move them, and they bounce off a floor and the frame's edges.
// They are drawn as dots, soft discs, streaks (motion blurred), squares or sprites from a picture or clip, with
// colour, size, opacity and spin over their life, flat or in 3D through the 3D titles' lens with nearer particles
// over farther ones.
//
// The simulation runs from the clip's start (less its pre-roll) at a fixed step from a seed, keeping its state every
// 16 frames, so a frame is the same however it is reached: scrubbed to, played, or rendered alone elsewhere. Every
// setting can be keyframed or given an expression (an emission rate of `audio(1) * 2000` follows the sound); the
// simulation reads them each frame and blends between frames.
#pragma once

#include <cstdint>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

// Positions are in sequence pixels from the frame's centre, x right, y down and z towards the viewer; speeds in pixels
// a second.
struct Particle {
    uint64_t id = 0;  // birth order: the burst's first, then the rate's
    double x = 0, y = 0, z = 0;
    double vx = 0, vy = 0, vz = 0;
    double age = 0, life = 1;  // seconds
    uint64_t outside = 0;      // the walls it was born outside of and has not come inside yet (Particles.cpp)
};

struct ParticleState {
    int64_t step = 0;       // fixed steps since time 0 (negative in the pre-roll)
    double due = 0;         // the birth rate's integral so far (particles)
    uint64_t rateBorn = 0;  // particles born from the rate
    uint64_t nextId = 0;
    std::vector<Particle> live;
};

// The particles at clip frame t of a "particles" generator in a `sw` x `sh` sequence at `fps`. Expressions in its
// settings are worked out in the thread's ExpressionScope.
ParticleState particleState(const Effect& g, FrameTime t, int sw, int sh, double fps);
// Forgets every kept state (tests, and memory pressure).
void clearParticleCache();

// The turbulence field's velocity (strength 1) at a point in sequence pixels and time in seconds, its swirls `scale`
// pixels across, changing at `evolve`: the 2D field is the rotated gradient of a noise stream function, the 3D one the
// curl of a noise vector potential, so neither has any divergence.
void curlNoise2(double x, double y, double t, double scale, double evolve, uint32_t seed, double out[2]);
void curlNoise3(double x, double y, double z, double t, double scale, double evolve, uint32_t seed, double out[3]);

// The generator's picture at clip frame t, w x h, for a `sw` x `sh` sequence (the simulation's space, whatever size it
// is drawn at). `bands` splits the drawing into that many bands on threads of their own (0: the shared pool's), which
// gives the same picture.
Image renderParticles(const Effect& g, FrameTime t, int w, int h, int sw, int sh, double fps, int bands = 0);

}  // namespace montage
