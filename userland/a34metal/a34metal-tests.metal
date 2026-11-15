// Shaders of our own for metal-probe's end-to-end tests (--compute, --draw), compiled in CI by
// Xcode's metal compiler for iOS 16 into a34metal-tests.metallib (build.sh). Kept simple on
// purpose: no buffers in the draw, one buffer in the kernel, so that a failure is the driver's
// and not the test's.
#include <metal_stdlib>
using namespace metal;

// every element i of out[0..grid) becomes 3i + 7
kernel void a34_fill(device uint *out [[buffer(0)]], uint id [[thread_position_in_grid]])
{
	out[id] = id * 3u + 7u;
}

struct A34Varyings {
	float4 position [[position]];
	float4 color;
};

// one triangle, no vertex buffer: (-0.5,-0.5) (0.5,-0.5) (0,0.5), red
vertex A34Varyings a34_tri_vs(uint vid [[vertex_id]])
{
	A34Varyings out;
	float2 p = vid == 0 ? float2(-0.5, -0.5) : vid == 1 ? float2(0.5, -0.5) : float2(0.0, 0.5);

	out.position = float4(p, 0.0, 1.0);
	out.color = float4(1.0, 0.0, 0.0, 1.0);
	return out;
}

fragment float4 a34_tri_fs(A34Varyings in [[stage_in]])
{
	return in.color;
}
