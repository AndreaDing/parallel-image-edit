#include "HalideBuffer.h"
#include "halide_benchmark.h"

#include "Halide.h"
#include "halide_image_io.h"
#include "cycletimer.h"

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <float.h>

using namespace Halide;
using namespace Halide::Tools;
using namespace std;

#define ITERATION 40000
#define USE_GPU 0

enum position_tag {INSIDE_MASK, BOUNDRY, OUTSIDE};

Target find_gpu_target();
int main(int argc, char** argv) {
    string source_image = "../input/1/source.png";
    string mask = "../input/1/mask.png";
    string target_image = "../input/1/target.png";

    cout<<" source_image   : "<<source_image<<endl;
    cout<<" target_image   : "<<target_image<<endl;
    cout<<" Mask name   : "<<mask <<endl;

    // preset the boundbox
    int boundBoxMinX_value = 66;
    int boundBoxMinY_value = 204;
    int boundBoxMaxX_value = 132;
    int boundBoxMaxY_value = 265; 
    int boundBoxWidth = 66;
    int boundBoxHeight = 61;

    Buffer<uint8_t> msourceImage = load_image(source_image);
    Buffer<uint8_t> mtargetImage = load_image(target_image);
    Buffer<uint8_t> mmask = load_image(mask);

    Var x,y,c;
    Expr value_source = msourceImage(x,y,c);
    Expr value_target = mtargetImage(x,y,c);
    Expr value_mask = mmask(x,y,c);

    value_mask = cast<float>(value_mask);
    
    Func extract_boundary;
    Func real_extract_boundary;
    Func merge_without_blend;
    Func tmp_calculate_target;
    Func calculate_target;

    extract_boundary(x, y, c) = value_mask;
    real_extract_boundary(x, y, c) = select(x==0||y==0||x==mmask.width()-1||y==mmask.height()-1, OUTSIDE*1.0f,    
                                        extract_boundary(x,y,c)==255 &&  
                                        extract_boundary(Halide::min(x+1, mmask.width()-1),y,c)==255&& 
                                        extract_boundary(Halide::max(x-1, 0),y,c)==255&&
                                        extract_boundary(x,Halide::min(y+1, mmask.height()-1),c)==255&& 
                                        extract_boundary(x,Halide::max(y-1, 0),c)==255, INSIDE_MASK*1.0f,
                                        extract_boundary(x,y,c)==255, BOUNDRY*1.0f,
                                        OUTSIDE);
    Buffer<float> boundary_array = real_extract_boundary.realize(mmask.width(), mmask.height(), mmask.channels());

    merge_without_blend(x,y,c) = cast<float>(value_source);
    merge_without_blend(x,y,c) = select(boundary_array(x,y,c)==INSIDE_MASK, cast<float>(mtargetImage(x,y,c)), merge_without_blend(x,y,c));
    Buffer<float> image_to_blend_f(boundBoxWidth, boundBoxHeight, mmask.channels());
    image_to_blend_f.set_min(boundBoxMinX_value+1, boundBoxMinY_value+1);
    merge_without_blend.realize(image_to_blend_f);
    image_to_blend_f.set_min(0,0);

    tmp_calculate_target(x, y, c) = cast<float>(value_target);
    calculate_target(x, y, c) = 4 * tmp_calculate_target(x, y, c) - tmp_calculate_target(x+1, y, c)- tmp_calculate_target(x-1, y, c)- tmp_calculate_target(x, y+1, c)- tmp_calculate_target(x, y-1, c);  
    Buffer<float> target_value_f(boundBoxWidth, boundBoxHeight, mmask.channels());
    target_value_f.set_min(boundBoxMinX_value+1, boundBoxMinY_value+1);
    calculate_target.realize(target_value_f);
    target_value_f.set_min(0,0);

    // to avoid calculation beyond bound
    Buffer<uint8_t> output(boundBoxWidth-1, boundBoxHeight-1, 3);

    double t3 = currentSeconds();
    RDom r(1, boundBoxWidth-2, 1, boundBoxHeight-2, 1, ITERATION);
    Func poisson_jacobi;
    Var t;
    poisson_jacobi(x,y,c,t) =  cast<float>(image_to_blend_f(x,y,c));
    poisson_jacobi(r.x, r.y, c, r.z) = 0.25f * target_value_f(r.x, r.y, c)
             +0.25f * (poisson_jacobi(r.x-1,r.y,c,r.z-1)
             +poisson_jacobi(r.x,r.y-1,c,r.z-1)
             +poisson_jacobi(r.x+1,r.y,c,r.z-1)
             +poisson_jacobi(r.x,r.y+1,c,r.z-1));
    
    Func final_image;
    final_image(x,y,c) = cast<uint8_t>(poisson_jacobi(x,y,c,ITERATION));

    Var xo,yo,xi,yi;
    Var xa,ya,xb,yb;
    Var co,ci;
    
    poisson_jacobi.compute_root();

    if(USE_GPU){
        poisson_jacobi.gpu_tile(x,y,xa,ya,xb,yb,4,4);
        final_image.gpu_tile(x,y,xo,yo,xi,yi,4,4);
        Target target = find_gpu_target();
        final_image.print_loop_nest();
        final_image.compile_jit(target);
    }

    
    final_image.realize(output);
    printf("Running Time: %gms\n", (currentSeconds()-t3)*1000);
    save_image(output, "finalimage.png");
}

// referenced from Halide/tutorial/lesson_12_using_the_gpu.cpp
Target find_gpu_target() {
    // Start with a target suitable for the machine you're running this on.
    Target target = get_host_target();

    std::vector<Target::Feature> features_to_try;
    if (target.os == Target::Windows) {
        // Try D3D12 first; if that fails, try OpenCL.
        if (sizeof(void*) == 8) {
            // D3D12Compute support is only available on 64-bit systems at present.
            features_to_try.push_back(Target::D3D12Compute);
        }
        features_to_try.push_back(Target::OpenCL);
    } else if (target.os == Target::OSX) {
        // OS X doesn't update its OpenCL drivers, so they tend to be broken.
        // CUDA would also be a fine choice on machines with NVidia GPUs.
        features_to_try.push_back(Target::Metal);
    } else {
        features_to_try.push_back(Target::OpenCL);
    }
    // Uncomment the following lines to also try CUDA:
    //

    for (Target::Feature f : features_to_try) {
        Target new_target = target.with_feature(f);
        if (host_supports_target_device(new_target)) {
            return new_target;
        }
    }
}