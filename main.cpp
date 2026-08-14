// ============================================================================
//  A quick ray-marching (sphere-tracing) renderer that draws a fiery explosion.
// 
//  The whole scene is ONE implicit surface, described by a signed distance function (SDF):
//
//      signed_distance(p) < 0   ->  p is inside  the object
//      signed_distance(p) = 0   ->  p is exactly on the surface
//      signed_distance(p) > 0   ->  p is outside the object
//
//  The object is a sphere whose radius is nudged by fractal noise, so the surface looks like a churning ball of fire:
//  
//      signed_distance(p) = length(p) - (sphere_radius + displacement(p))
//
//  Rendering pipeline, per pixel:
//      1. build a camera ray                        							(main)
//      2. march along it until the SDF goes < 0     							(sphere_trace)
//      3. how deep the hit is inside the original sphere gives a "temperature" (main)
//      4. temperature -> colour                     							(palette_fire)
//      5. shade with one light                      							(distance_field_normal)
//
//  NOTE ON STYLE: this impl deliberately keeps every formula written out in
//  its unevaluated, symbolic form (e.g. 1.0f/16.0f instead of 0.0625f) so the
//  structure of the maths stays visible. It is not optimised, and it is not
//  meant to be.
//
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <vector>

#include "vec.hpp"

// ----------------------------------------------------------------------------
//  Scene constants
// ----------------------------------------------------------------------------

// The explosion is a nudged sphere centred at the origin. Every part of it
// fits inside a sphere of this radius, because the noise only ever pushes the
// surface inwards (see signed_distance below).
const float sphere_radius = 1.5f;

// How far, in world units, the noise is allowed to dent the sphere inwards.
const float noise_amplitude = 1.0f;

const float pi = 3.14159265358979323846f;

// ----------------------------------------------------------------------------
//  Generic helper: linear interpolation
//
//      lerp(a, b, t) = a + (b - a) * t     with t clamped to [0, 1]
//
//  Works both for scalars (float) and for vectors (Vec3f), because vec.hpp
//  provides vector + vector, vector - vector and vector * scalar.
// ----------------------------------------------------------------------------
template <typename T>
inline T linear_interpolation(const T &start_value, const T &end_value, const float weight)
{
    const float clamped_weight = std::clamp(weight, 0.0f, 1.0f);
    return start_value + (end_value - start_value) * clamped_weight;
}

// ============================================================================
//  1. NOISE
// ============================================================================

// ----------------------------------------------------------------------------
//  A cheap pseudo random number generator: one float in, one float in [0, 1) out.
//
//      hash(seed) = fractional_part( sin(seed) * huge_amplitude )
//
//  sin() is bounded, so multiplying it by a huge amplitude and then throwing
//  away the integer part gives digits that jump around wildly for tiny changes
//  of the seed. That is all the "randomness" we need.
//
//  IMPORTANT: the sine MUST be evaluated in double precision. Multiplying by
//  ~44000 shifts the value left by about 15 binary digits, so the fractional
//  part we keep is built from the *last* bits of the sine. A single-precision
//  sine simply does not have enough of those bits left, and the "random"
//  values would come out coarsely quantised (in steps of about 1/256) and
//  different from the original. The original code gets this by accident: its
//  unqualified sin() call resolves to the double-precision ::sin. Here it is
//  spelled out on purpose.
// ----------------------------------------------------------------------------
float hash_to_unit_range(const float seed)
{
    const float huge_amplitude = 43758.5453f;

    const float scrambled = static_cast<float>(std::sin(static_cast<double>(seed)) * huge_amplitude);
    const float fractional_part = scrambled - std::floor(scrambled);

    return fractional_part;
}

// ----------------------------------------------------------------------------
//  Value noise lives on the integer lattice of 3D space: every lattice corner
//  (integer x, y, z) is given a pseudo random value, and any point in between
//  is a blend of the 8 corners of the cell that contains it.
//
//  A corner needs to be turned into a single number to feed the hash. We do it
//  with a dot product against three de-correlating strides:
//
//      corner_id(ix, iy, iz) = ix*stride_x + iy*stride_y + iz*stride_z
//
//  The strides are arbitrary; they just need to be large and unrelated enough
//  that neighbouring corners get very different ids.
// ----------------------------------------------------------------------------
const float lattice_stride_x = 1.0f;
const float lattice_stride_y = 57.0f;
const float lattice_stride_z = 113.0f;

// Pseudo random value of the corner that sits (offset_x, offset_y, offset_z)
// lattice steps away from the corner identified by cell_corner_id.
float lattice_corner_value(const float cell_corner_id,
                           const float offset_x,
                           const float offset_y,
                           const float offset_z)
{
    return hash_to_unit_range(cell_corner_id
                              + offset_x * lattice_stride_x
                              + offset_y * lattice_stride_y
                              + offset_z * lattice_stride_z);
}

float value_noise(const Vec3f &point)
{
    // Split the point into "which lattice cell" and "where inside that cell".
    const Vec3f cell_corner(std::floor(point.x), std::floor(point.y), std::floor(point.z));
    Vec3f position_in_cell(point.x - cell_corner.x,   // each component in [0, 1)
                           point.y - cell_corner.y,
                           point.z - cell_corner.z);

    // ---- smoothing of the blend weights -------------------------------------
    //
    // The original tinykaboom line is:
    //
    //     f = f * (f * (Vec3f(3,3,3) - f*2));
    //
    // In geometry.h, vector * vector resolves to the DOT PRODUCT, so the inner
    // parenthesis collapses to a single scalar and the line actually computes
    //
    //     position_in_cell  <-  position_in_cell * dot(position_in_cell, 3 - 2*position_in_cell)
    //
    // and NOT the component-wise Hermite smoothstep  f_i <- f_i * f_i * (3 - 2*f_i)
    // that this pattern normally stands for. That is a large part of why the
    // original author calls this "a bad noise function with lots of artifacts".
    // The behaviour is reproduced exactly here so the rendered image is
    // unchanged; see the note at the bottom of this file for the intended
    // component-wise version.
    const Vec3f three(3.0f, 3.0f, 3.0f);
    const float smoothing = dot(position_in_cell, three - position_in_cell * 2.0f);
    position_in_cell = position_in_cell * smoothing;

    // Id of the cell's lowest corner, i.e. the (0,0,0) corner of this cell.
    const Vec3f lattice_strides(lattice_stride_x, lattice_stride_y, lattice_stride_z);
    const float cell_corner_id = dot(cell_corner, lattice_strides);

    // ---- trilinear blend of the 8 corners -----------------------------------
    // First interpolate along x (4 edges), then along y (2 faces), then along z.
    const float edge_y0_z0 = linear_interpolation(lattice_corner_value(cell_corner_id, 0, 0, 0),
                                                  lattice_corner_value(cell_corner_id, 1, 0, 0),
                                                  position_in_cell.x);
    const float edge_y1_z0 = linear_interpolation(lattice_corner_value(cell_corner_id, 0, 1, 0),
                                                  lattice_corner_value(cell_corner_id, 1, 1, 0),
                                                  position_in_cell.x);
    const float edge_y0_z1 = linear_interpolation(lattice_corner_value(cell_corner_id, 0, 0, 1),
                                                  lattice_corner_value(cell_corner_id, 1, 0, 1),
                                                  position_in_cell.x);
    const float edge_y1_z1 = linear_interpolation(lattice_corner_value(cell_corner_id, 0, 1, 1),
                                                  lattice_corner_value(cell_corner_id, 1, 1, 1),
                                                  position_in_cell.x);

    const float face_z0 = linear_interpolation(edge_y0_z0, edge_y1_z0, position_in_cell.y);
    const float face_z1 = linear_interpolation(edge_y0_z1, edge_y1_z1, position_in_cell.y);

    return linear_interpolation(face_z0, face_z1, position_in_cell.z);
}

// ----------------------------------------------------------------------------
//  A fixed 3x3 matrix applied to the sample point before the noise octaves are
//  summed. Its only job is to twist sample space away from the coordinate axes,
//  so the octaves of the lattice-aligned noise do not stack up into a visible
//  grid pattern.
//
//  Matrix times vector: component i of the result is the dot product of row i
//  with the vector.
//
//      | r0x r0y r0z |   | vx |     | dot(row_0, v) |
//      | r1x r1y r1z | * | vy |  =  | dot(row_1, v) |
//      | r2x r2y r2z |   | vz |     | dot(row_2, v) |
// ----------------------------------------------------------------------------
Vec3f rotate_sample_space(const Vec3f &v)
{
    const Vec3f row_0( 0.00f,  0.80f,  0.60f);
    const Vec3f row_1(-0.80f,  0.36f, -0.48f);
    const Vec3f row_2(-0.60f, -0.48f,  0.64f);

    return Vec3f(dot(row_0, v), dot(row_1, v), dot(row_2, v));
}

// ----------------------------------------------------------------------------
//  Fractal Brownian motion: sum several "octaves" of the same noise function.
//  Each octave is sampled at a higher frequency (finer detail) and added with a
//  smaller amplitude (weaker contribution):
//
//      fbm(p) = ( sum over octaves of  amplitude_k * noise(frequency_k * p) )
//               / ( sum of amplitudes )
//
//  Dividing by the sum of the amplitudes renormalises the result back to [0, 1],
//  because each noise() call is already in [0, 1].
//
//  Amplitudes halve every octave. Frequencies grow by non-integer factors so
//  that the lattices of different octaves never line up with each other.
// ----------------------------------------------------------------------------
float fractal_brownian_motion(const Vec3f &point)
{
    const float amplitude_octave_1 = 1.0f /  2.0f;
    const float amplitude_octave_2 = 1.0f /  4.0f;
    const float amplitude_octave_3 = 1.0f /  8.0f;
    const float amplitude_octave_4 = 1.0f / 16.0f;

    const float frequency_gain_1_to_2 = 2.32f;
    const float frequency_gain_2_to_3 = 3.03f;
    const float frequency_gain_3_to_4 = 2.61f;

    Vec3f sample_point = rotate_sample_space(point);

    float accumulated_noise = 0.0f;

    accumulated_noise += amplitude_octave_1 * value_noise(sample_point);
    sample_point = sample_point * frequency_gain_1_to_2;

    accumulated_noise += amplitude_octave_2 * value_noise(sample_point);
    sample_point = sample_point * frequency_gain_2_to_3;

    accumulated_noise += amplitude_octave_3 * value_noise(sample_point);
    sample_point = sample_point * frequency_gain_3_to_4;

    accumulated_noise += amplitude_octave_4 * value_noise(sample_point);

    const float total_amplitude = amplitude_octave_1
                                + amplitude_octave_2
                                + amplitude_octave_3
                                + amplitude_octave_4;   // = 15/16 = 0.9375

    return accumulated_noise / total_amplitude;
}

// ============================================================================
//  2. COLOUR
// ============================================================================

// ----------------------------------------------------------------------------
//  A gradient with five colour stops, evenly spaced over [0, 1]:
//
//      0.00        0.25        0.50        0.75        1.00
//      gray ---- darkgray ---- red ------- orange ---- yellow
//     (coldest)                                       (hottest)
//
//  Inside segment k (which spans [k*w, (k+1)*w] with w = 1/4) the local
//  interpolation weight is
//
//      local_weight = (temperature - k*w) / w
//
//  which runs from 0 at the start of the segment to 1 at its end. (The original
//  writes this pre-simplified as "x*4 - k"; it is the same expression.)
// ----------------------------------------------------------------------------
Vec3f palette_fire(const float temperature)
{
    // Note that the hot end of the palette has components > 1: the colour is
    // "over-bright" so that it stays saturated after the light attenuation
    // applied later, and it gets clamped only when written to the image.
    const Vec3f yellow  (1.7f, 1.3f, 1.0f);
    const Vec3f orange  (1.0f, 0.6f, 0.0f);
    const Vec3f red     (1.0f, 0.0f, 0.0f);
    const Vec3f darkgray(0.2f, 0.2f, 0.2f);
    const Vec3f gray    (0.4f, 0.4f, 0.4f);

    const int   segment_count = 4;                                       // 5 colour stops -> 4 segments
    const float segment_width = 1.0f / static_cast<float>(segment_count);

    const float clamped_temperature = std::clamp(temperature, 0.0f, 1.0f);

    if (clamped_temperature < 1 * segment_width)
    {
        const float local_weight = (clamped_temperature - 0 * segment_width) / segment_width;
        return linear_interpolation(gray, darkgray, local_weight);
    }
    if (clamped_temperature < 2 * segment_width)
    {
        const float local_weight = (clamped_temperature - 1 * segment_width) / segment_width;
        return linear_interpolation(darkgray, red, local_weight);
    }
    if (clamped_temperature < 3 * segment_width)
    {
        const float local_weight = (clamped_temperature - 2 * segment_width) / segment_width;
        return linear_interpolation(red, orange, local_weight);
    }

    const float local_weight = (clamped_temperature - 3 * segment_width) / segment_width;
    return linear_interpolation(orange, yellow, local_weight);
}

// ============================================================================
//  3. THE IMPLICIT SURFACE
// ============================================================================

// ----------------------------------------------------------------------------
//  Signed distance function of the explosion.
//
//  A plain sphere of radius R centred at the origin is simply
//
//      signed_distance(p) = |p| - R
//
//  Here the radius is made to wobble from point to point:
//
//      displacement(p)    = -fbm(noise_frequency * p) * noise_amplitude
//      local_radius(p)    = sphere_radius + displacement(p)
//      signed_distance(p) = |p| - local_radius(p)
//
//  fbm() returns values in [0, 1], so displacement is always <= 0: the noise
//  only ever carves dents *into* the sphere, never bulges out of it. That is
//  what makes the bounding-sphere early-out in sphere_trace() safe.
//
//  This is only an approximation of a true distance (it is not 1-Lipschitz),
//  which is why the marching step below is deliberately conservative.
// ----------------------------------------------------------------------------
float signed_distance(const Vec3f &point)
{
    const float noise_frequency = 3.4f;   // how many noise cells fit per world unit

    const float displacement  = -fractal_brownian_motion(point * noise_frequency) * noise_amplitude;
    const float local_radius  = sphere_radius + displacement;

    return length(point) - local_radius;
}

// ----------------------------------------------------------------------------
//  Ray marching / sphere tracing.
//
//  Walk along the ray in steps and stop as soon as the signed distance says we
//  are inside the object. The step length is derived from the current distance
//  to the surface: far away we may take big steps, close by we take small ones.
//
//  Returns true and writes the hit point into hit_position; false on a miss.
// ----------------------------------------------------------------------------
bool sphere_trace(const Vec3f &ray_origin, const Vec3f &ray_direction, Vec3f &hit_position)
{
    // ---- conservative early-out --------------------------------------------
    // Everything fits inside the sphere of radius sphere_radius centred at the
    // origin, so a ray that misses that bounding sphere cannot hit anything.
    //
    // For a ray with a UNIT direction d starting at o, the squared distance
    // from the origin to the closest point of the ray line is, by Pythagoras,
    //
    //      closest_approach^2 = |o|^2 - (o . d)^2
    //
    // because (o . d) is the length of the projection of o onto the ray.
    // This is purely a speed-up; removing it would not change the image.
    const float origin_distance_squared     = dot(ray_origin, ray_origin);
    const float projection_onto_ray         = dot(ray_origin, ray_direction);
    const float closest_approach_squared    = origin_distance_squared - projection_onto_ray * projection_onto_ray;

    if (closest_approach_squared > sphere_radius * sphere_radius)
    {
        return false;
    }

    // ---- the march ----------------------------------------------------------
    const size_t max_marching_steps = 128;
    const float  step_safety_factor = 0.1f;   // fraction of the reported distance we dare to move
    const float  minimum_step       = 0.01f;  // never crawl slower than this, or we would never finish

    hit_position = ray_origin;

    for (size_t step = 0; step < max_marching_steps; step++)
    {
        const float distance_to_surface = signed_distance(hit_position);

        if (distance_to_surface < 0)   // we are inside the object: that is our hit
        {
            return true;
        }

        const float step_length = std::max(distance_to_surface * step_safety_factor, minimum_step);
        hit_position = hit_position + ray_direction * step_length;
    }

    return false;   // ran out of steps without ever getting inside
}

// ----------------------------------------------------------------------------
//  Surface normal of an implicit surface = normalised gradient of its SDF.
//
//  The gradient is approximated with forward finite differences:
//
//      dF/dx  ~=  ( F(p + (eps, 0, 0)) - F(p) ) / eps
//      dF/dy  ~=  ( F(p + (0, eps, 0)) - F(p) ) / eps
//      dF/dz  ~=  ( F(p + (0, 0, eps)) - F(p) ) / eps
//
//  The common 1/eps factor is the same for all three components, so it only
//  scales the vector and disappears when we normalise it. That is why it is
//  omitted below.
//
//  This is very sensitive to eps: too small and the noise makes it jitter, too
//  large and the surface looks smoothed over.
// ----------------------------------------------------------------------------
Vec3f distance_field_normal(const Vec3f &position)
{
    const float eps = 0.1f;

    const float distance_here = signed_distance(position);

    const float gradient_x = signed_distance(position + Vec3f(eps, 0.0f, 0.0f)) - distance_here;
    const float gradient_y = signed_distance(position + Vec3f(0.0f, eps, 0.0f)) - distance_here;
    const float gradient_z = signed_distance(position + Vec3f(0.0f, 0.0f, eps)) - distance_here;

    return normalized(Vec3f(gradient_x, gradient_y, gradient_z));
}

// ============================================================================
//  4. RENDERING
// ============================================================================

int main()
{
    // ---- image ---------------------------------------------------------------
    const int image_width  = 640;
    const int image_height = 480;

    // ---- camera --------------------------------------------------------------
    // A pinhole camera sitting at (0, 0, 3) and looking down the -z axis.
    const Vec3f camera_position(0.0f, 0.0f, 3.0f);
    const float vertical_field_of_view = pi / 3.0f;   // radians (= 60 degrees)

    // Where to put the image plane so that its full height is seen under exactly
    // vertical_field_of_view. Half the plane subtends half the angle:
    //
    //      tan(fov / 2) = (image_height / 2) / image_plane_distance
    //  =>  image_plane_distance = (image_height / 2) / tan(fov / 2)
    //
    // Distances are in pixels here, which is fine: only the ray's DIRECTION
    // matters and that is normalised afterwards.
    const float image_plane_distance =
        (image_height / 2.0f) / std::tan(vertical_field_of_view / 2.0f);

    // ---- lighting and background --------------------------------------------
    const Vec3f light_position(10.0f, 10.0f, 10.0f);
    const float ambient_light_intensity = 0.4f;       // floor so nothing is pitch black
    const Vec3f background_color(0.2f, 0.7f, 0.8f);

    // ---- how the dent depth is mapped onto the fire palette ------------------
    const float palette_offset = -0.2f;
    const float palette_scale  =  2.0f;

    std::vector<Vec3f> framebuffer(static_cast<size_t>(image_width) * image_height);

#pragma omp parallel for
    for (int pixel_y = 0; pixel_y < image_height; pixel_y++)
    {
        for (int pixel_x = 0; pixel_x < image_width; pixel_x++)
        {
            // Sample through the CENTRE of the pixel, hence the + 0.5.
            const float pixel_center_x = static_cast<float>(pixel_x) + 0.5f;
            const float pixel_center_y = static_cast<float>(pixel_y) + 0.5f;

            // Pixel position measured from the middle of the image plane.
            // The y axis is negated because image rows grow downwards while the
            // world's y axis grows upwards; this flips the image the right way up.
            const float ray_direction_x =  (pixel_center_x - image_width  / 2.0f);
            const float ray_direction_y = -(pixel_center_y - image_height / 2.0f);
            const float ray_direction_z = -image_plane_distance;   // camera looks towards -z

            const Vec3f ray_direction =
                normalized(Vec3f(ray_direction_x, ray_direction_y, ray_direction_z));

            const size_t pixel_index = static_cast<size_t>(pixel_x) + static_cast<size_t>(pixel_y) * image_width;

            Vec3f hit_position;
            if (!sphere_trace(camera_position, ray_direction, hit_position))
            {
                framebuffer[pixel_index] = background_color;
                continue;
            }

            // How deep, relative to the maximum possible dent, the hit point lies
            // inside the unperturbed sphere:
            //
            //      0 -> right on the original sphere surface (coolest, outer smoke)
            //      1 -> as deep as the noise can ever carve   (hottest, the core)
            const float dent_depth = (sphere_radius - length(hit_position)) / noise_amplitude;

            const float fire_temperature = (dent_depth + palette_offset) * palette_scale;
            const Vec3f surface_color    = palette_fire(fire_temperature);

            // Lambert shading with a single point light:
            //
            //      intensity = max(ambient, cos(angle between normal and light))
            //                = max(ambient, dot(normal, light_direction))
            //
            // (both vectors are unit length, so the dot product IS the cosine)
            const Vec3f light_direction = normalized(light_position - hit_position);
            const Vec3f surface_normal  = distance_field_normal(hit_position);
            const float light_intensity =
                std::max(ambient_light_intensity, dot(light_direction, surface_normal));

            framebuffer[pixel_index] = surface_color * light_intensity;
        }
    }

    // ---- write the framebuffer as a binary PPM (P6) --------------------------
    // Header: magic number, width, height, maximum channel value; then the raw
    // RGB bytes, one byte per channel.
    const int   max_channel_value = 255;
    std::ofstream output_file("./out.ppm", std::ios::binary);
    output_file << "P6\n" << image_width << " " << image_height << "\n" << max_channel_value << "\n";

    for (const Vec3f &color : framebuffer)
    {
        for (size_t channel = 0; channel < Vec3f::size; channel++)
        {
            // Colours may be over-bright (> 1), so map to [0, 255] and clamp.
            // Note the clamping happens on the INTEGER: converting an
            // out-of-range float straight to a char is undefined behaviour.
            const int channel_value = std::clamp(static_cast<int>(max_channel_value * color[channel]),
                                                 0, max_channel_value);
            output_file.put(static_cast<char>(static_cast<unsigned char>(channel_value)));
        }
    }

    output_file.close();

    return 0;
}

// ============================================================================
//  APPENDIX: the smoothstep in value_noise()
//
//  If you ever want the *intended* smoothing (component-wise Hermite), it would
//  read like this, and it needs a component-wise multiply, which vec.hpp does
//  not currently provide:
//
//      for (size_t axis = 0; axis < Vec3f::size; axis++) {
//          const float t = position_in_cell[axis];
//          position_in_cell[axis] = t * t * (3.0f - 2.0f * t);
//      }
//
//  It produces a different (arguably nicer) image, which is why it is left out
//  of the main code path: this file is meant to be a faithful, readable copy of
//  the original.
// ============================================================================