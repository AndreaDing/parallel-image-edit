// Halide tutorial lesson 21: Auto-Scheduler

// So far we have written Halide schedules by hand, but it is also possible to
// ask Halide to suggest a reasonable schedule. We call this auto-scheduling.
// This lesson demonstrates how to use the auto-scheduler to generate a
// copy-pasteable CPU schedule that can be subsequently improved upon.

// On linux or os x, you can compile and run it like so:

// g++ lesson_21_auto_scheduler_generate.cpp ../tools/GenGen.cpp -g -std=c++11 -fno-rtti -I ../include -L ../bin -lHalide -lpthread -ldl -o lesson_21_generate
// export LD_LIBRARY_PATH=../bin   # For linux
// export DYLD_LIBRARY_PATH=../bin # For OS X
// ./lesson_21_generate -o . -g auto_schedule_gen -f auto_schedule_false -e static_library,h,schedule target=host auto_schedule=false
// ./lesson_21_generate -o . -g auto_schedule_gen -f auto_schedule_true -e static_library,h,schedule target=host auto_schedule=true machine_params=32,16777216,40
// g++ lesson_21_auto_scheduler_run.cpp -std=c++11 -I ../include -I ../tools auto_schedule_false.a auto_schedule_true.a -ldl -lpthread -o lesson_21_run
// ./lesson_21_run

// If you have the entire Halide source tree, you can also build it by
// running:
//    make tutorial_lesson_21_auto_scheduler_run
// in a shell with the current directory at the top of the halide
// source tree.

#include "Halide.h"
#include <stdio.h>
#include <stdlib.h>
#include <float.h>

using namespace Halide;

#ifndef HALF_PATCH
#define HALF_PATCH 7
#endif
#define NUM_ITERATIONS 10
#define RADIUS 5
#define STRIDE 1

struct MapEntry {
    Expr dx;
    Expr dy;
    Expr sx;
    Expr sy;
    Expr d;

    MapEntry(Tuple t) 
    : dx(t[0]), dy(t[1]), sx(t[2]), sy(t[2]), d(t[4]) {}

    MapEntry(Expr dx_, Expr dy_, Expr sx_, Expr sy_, Expr d_) 
    : dx(dx_), dy(dy_), sx(sx_), sy(sy_), d(d_) {}

    MapEntry(FuncRef t) : MapEntry(Tuple(t)) {}

    operator Tuple() const { return {dx, dy, sx, sy, d}; }

    Expr get_dx() { return dx; }

    Expr get_dy() { return dy; }

    Expr get_sx() { return sx; }

    Expr get_sy() { return sy; }

    Expr get_d() { return d; }
};


// We will define a generator to auto-schedule.
class AutoScheduled : public Halide::Generator<AutoScheduled> {
public:
    Input<Buffer<uint8_t>>  dst_u8{"dst_u8", 3};
    Input<Buffer<uint8_t>>  src_u8{"src_u8", 3};

    Output<Buffer<uint8_t>> output{"output", 3};

    inline Expr sum_squared_diff(Tuple a, Tuple b)
    {
        return sqrt(
            (a[0] - b[0]) * (a[0] - b[0]) +
            (a[1] - b[1]) * (a[1] - b[1]) +
            (a[2] - b[2]) * (a[2] - b[2])
        );
    }

    Expr patch_distance(Func dst, Func src, Expr dx, Expr dy, Expr sx, Expr sy, 
        Expr width, Expr height)
    {
        Expr distance(0.f);
        for (int j = -HALF_PATCH; j <= HALF_PATCH; j++) {
            for (int i = -HALF_PATCH; i <= HALF_PATCH; i++) {
                Expr dx1 = min(width - 1, max(0, dx + i));
                Expr dy1 = min(height - 1, max(0, dy + i));
                Expr sx1 = min(width - 1, max(0, sx + i));
                Expr sy1 = min(height - 1, max(0, sy + i));

                Tuple dpixel = dst(dx1, dy1);
                Tuple spixel = src(sx1, sy1);
                distance += sum_squared_diff(dpixel, spixel);
            }
        }
        return distance;
    }

    Tuple propagate(Tuple cur, Tuple left, Tuple up, Func dst, Func src, Expr width, Expr height)
    {
        Expr dx = cur[0];
        Expr dy = cur[1];
        Expr sx = cur[2];
        Expr sy = cur[3];
        Expr d = cur[4];
        Expr sx_left = min(width - 1, max(0, left[2] + 1));
        Expr sy_left = left[3];
        Expr sx_up = up[2];
        Expr sy_up = min(height - 1, max(0, up[3] + 1));

        Expr left_d = patch_distance(dst, src, dx, dy, sx_left, sy_left, width, height);
        Expr up_d = patch_distance(dst, src, dx, dy, sx_up, sy_up, width, height);

        Expr new_sx = select(left_d <= d, 
            select(left_d <= up_d, sx_left, sx_up),
            select(d <= up_d, sx, sx_up));

        Expr new_sy = select(up_d <= d, 
            select(up_d <= left_d, sy_up, sy_left),
            select(d <= left_d, sy, sy_left));

        Expr new_d = select(d <= left_d, 
            select(d <= up_d, d, up_d),
            select(left_d <= up_d, left_d, up_d)
        );

        return Tuple(dx, dy, new_sx, new_sy, new_d);
    }

    Tuple random_search(Tuple cur, Tuple ran, Func dst, Func src, Expr width, Expr height)
    {
        Expr dx = cur[0];
        Expr dy = cur[1];
        Expr sx = cur[2];
        Expr sy = cur[3];
        Expr d = cur[4];

        Expr rx = max(0, min(ran[2], width - 1));
        Expr ry = max(0, min(ran[3], height - 1));
        Expr rd = patch_distance(dst, src, dx, dy, rx, ry, width, height);

        Expr new_sx = select(d <= rd, sx, rx);
        Expr new_sy = select(d <= rd, sy, ry);
        Expr new_d = select(d <= rd, d, rd);

        return Tuple(dx, dy, new_sx, new_sy, new_d);
    }

    Tuple nn_search(Tuple cur, Tuple left, Tuple up,
        Tuple ran1, Tuple ran2, Tuple ran3, Tuple ran4, Tuple ran5,
        Func dst, Func src, Expr width, Expr height)
    {
        cur = propagate(cur, left, up, dst, src, width, height);
        // cur = random_search(cur, ran1, dst, src, width, height);
        // cur = random_search(cur, ran2, dst, src, width, height);
        // cur = random_search(cur, ran3, dst, src, width, height);
        // cur = random_search(cur, ran4, dst, src, width, height);
        cur = random_search(cur, ran5, dst, src, width, height);
        return cur;
    }

    void generate() {
        Expr width = dst_u8.width();
        Expr height = dst_u8.height();

        Func dst_f("dst_f"), src_f("src_f");
        dst_f(x, y, c) = cast<float>(dst_u8(x, y, c)) / 255.f;
        src_f(x, y, c) = cast<float>(src_u8(x, y, c)) / 255.f;

        Func dst("dst"), src("src");
        dst(x, y) = Tuple(dst_f(x, y, 0), dst_f(x, y, 1), dst_f(x, y, 2));
        src(x, y) = Tuple(src_f(x, y, 0), src_f(x, y, 1), src_f(x, y, 2));

        Var t;
        map(x, y, t) = MapEntry(x, y, 
            random_int() % width, 
            random_int() % height, 
            FLT_MAX);

        RDom r(0, width, 0, height, 1, NUM_ITERATIONS);
        map(r.x, r.y, r.z) = nn_search(
            map(r.x, r.y, r.z - 1), 
            map(r.x - 1, r.y, r.z - 1),
            map(r.x, r.y - 1, r.z - 1),
            map(r.x + (random_int() % 10 - 5), r.y + (random_int() % 10 - 5), r.z - 1),
            map(r.x + (random_int() % 18 - 9), r.y + (random_int() % 18 - 9), r.z - 1),
            map(r.x + (random_int() % 24 - 12), r.y + (random_int() % 24 - 12), r.z - 1),
            map(r.x + (random_int() % 28 - 14), r.y + (random_int() % 28 - 14), r.z - 1),
            map(r.x + (random_int() % 30 - 15), r.y + (random_int() % 30 - 15), r.z - 1),
            dst, src, width, height);

        remap(x, y, c) = src_f(
            MapEntry(map(x, y, NUM_ITERATIONS)).get_sx() % width, 
            MapEntry(map(x, y, NUM_ITERATIONS)).get_sy() % height, 
            c);

        output(x, y, c) = cast<uint8_t>(remap(x, y, c) * 255);
    }

    void schedule() {
        if (auto_schedule) {
            dst_u8.set_estimates({{0, 1024}, {0, 1024}, {0, 3}});
            src_u8.set_estimates({{0, 1024}, {0, 1024}, {0, 3}});
            output.set_estimates({{0, 1024}, {0, 1024}, {0, 3}});
        } 
        else {
            map.compute_root()
                .parallel(y)
                .parallel(x);
            output.compute_root()
                .parallel(y)
                .parallel(x);
        }
    }
private:
    Var x{"x"}, y{"y"}, c{"c"};
    Func map;
    Func remap;
};

// As in lesson 15, we register our generator and then compile this
// file along with tools/GenGen.cpp.
HALIDE_REGISTER_GENERATOR(AutoScheduled, auto_schedule_gen)

// After compiling this file, see how to use it in
// lesson_21_auto_scheduler_run.cpp
