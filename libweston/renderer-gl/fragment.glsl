/*
 * Copyright 2012 Intel Corporation
 * Copyright 2015,2019,2021 Collabora, Ltd.
 * Copyright 2016 NVIDIA Corporation
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* GLSL version 1.00 ES, defined in gl-shaders.c */

/* For annotating shader compile-time constant arguments */
#define compile_const const

/*
 * Enumeration of shader variants, must match enum gl_shader_texture_variant.
 */
#define SHADER_VARIANT_RGBX     1
#define SHADER_VARIANT_RGBA     2
#define SHADER_VARIANT_Y_U_V    3
#define SHADER_VARIANT_Y_UV     4
#define SHADER_VARIANT_Y_XUXV   5
#define SHADER_VARIANT_XYUV     6
#define SHADER_VARIANT_SOLID    7
#define SHADER_VARIANT_EXTERNAL 8

/* enum gl_shader_color_curve */
#define SHADER_COLOR_CURVE_IDENTITY 0
#define SHADER_COLOR_CURVE_LUT_3x1D 1
#define SHADER_COLOR_CURVE_LINPOW 2
#define SHADER_COLOR_CURVE_POWLIN 3

/* enum gl_shader_color_mapping */
#define SHADER_COLOR_MAPPING_IDENTITY 0
#define SHADER_COLOR_MAPPING_3DLUT 1
#define SHADER_COLOR_MAPPING_MATRIX 2

#if DEF_VARIANT == SHADER_VARIANT_EXTERNAL
#extension GL_OES_EGL_image_external : require
#endif

#if DEF_COLOR_MAPPING == SHADER_COLOR_MAPPING_3DLUT
#extension GL_OES_texture_3D : require
#endif

#ifdef GL_FRAGMENT_PRECISION_HIGH
#define HIGHPRECISION highp
#else
#define HIGHPRECISION mediump
#endif

precision HIGHPRECISION float;

/*
 * These undeclared identifiers will be #defined by a runtime generated code
 * snippet.
 */
compile_const int c_variant = DEF_VARIANT;
compile_const bool c_input_is_premult = DEF_INPUT_IS_PREMULT;
compile_const bool c_green_tint = DEF_GREEN_TINT;
compile_const int c_color_pre_curve = DEF_COLOR_PRE_CURVE;
compile_const int c_color_mapping = DEF_COLOR_MAPPING;
compile_const int c_color_post_curve = DEF_COLOR_POST_CURVE;

compile_const bool c_need_color_pipeline =
	c_color_pre_curve != SHADER_COLOR_CURVE_IDENTITY ||
	c_color_mapping != SHADER_COLOR_MAPPING_IDENTITY ||
	c_color_post_curve != SHADER_COLOR_CURVE_IDENTITY;

vec4
yuva2rgba(vec4 yuva)
{
	vec4 color_out;
	float Y, su, sv;

	/* ITU-R BT.601 & BT.709 quantization (limited range) */

	/* Y = 255/219 * (x - 16/256) */
	Y = 1.16438356 * (yuva.x - 0.0625);

	/* Remove offset 128/256, but the 255/224 multiplier comes later */
	su = yuva.y - 0.5;
	sv = yuva.z - 0.5;

	/*
	 * ITU-R BT.601 encoding coefficients (inverse), with the
	 * 255/224 limited range multiplier already included in the
	 * factors for su (Cb) and sv (Cr).
	 */
	color_out.r = Y                   + 1.59602678 * sv;
	color_out.g = Y - 0.39176229 * su - 0.81296764 * sv;
	color_out.b = Y + 2.01723214 * su;

	color_out.a = yuva.w;

	return color_out;
}

#if DEF_VARIANT == SHADER_VARIANT_EXTERNAL
uniform samplerExternalOES tex;
#else
uniform sampler2D tex;
#endif

varying HIGHPRECISION vec2 v_texcoord;
uniform sampler2D tex1;
uniform sampler2D tex2;
uniform float view_alpha;
uniform vec4 unicolor;

#define MAX_CURVE_PARAMS 10
#define MAX_CURVESET_PARAMS (MAX_CURVE_PARAMS * 3)

uniform HIGHPRECISION sampler2D color_pre_curve_lut_2d;
uniform HIGHPRECISION vec2 color_pre_curve_lut_scale_offset;
uniform HIGHPRECISION float color_pre_curve_params[MAX_CURVESET_PARAMS];
uniform bool color_pre_curve_clamped_input;

uniform HIGHPRECISION sampler2D color_post_curve_lut_2d;
uniform HIGHPRECISION vec2 color_post_curve_lut_scale_offset;
uniform HIGHPRECISION float color_post_curve_params[MAX_CURVESET_PARAMS];
uniform bool color_post_curve_clamped_input;

#if DEF_COLOR_MAPPING == SHADER_COLOR_MAPPING_3DLUT
uniform HIGHPRECISION sampler3D color_mapping_lut_3d;
uniform HIGHPRECISION vec2 color_mapping_lut_scale_offset;
#endif
uniform HIGHPRECISION mat3 color_mapping_matrix;

vec4
sample_input_texture()
{
	vec4 yuva = vec4(0.0, 0.0, 0.0, 1.0);

	/* Producing RGBA directly */

	if (c_variant == SHADER_VARIANT_SOLID)
		return unicolor;

	if (c_variant == SHADER_VARIANT_RGBA ||
	    c_variant == SHADER_VARIANT_EXTERNAL) {
		return texture2D(tex, v_texcoord);
	}

	if (c_variant == SHADER_VARIANT_RGBX)
		return vec4(texture2D(tex, v_texcoord).rgb, 1.0);

	/* Requires conversion to RGBA */

	if (c_variant == SHADER_VARIANT_Y_U_V) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.y = texture2D(tex1, v_texcoord).x;
		yuva.z = texture2D(tex2, v_texcoord).x;

	} else if (c_variant == SHADER_VARIANT_Y_UV) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.yz = texture2D(tex1, v_texcoord).rg;

	} else if (c_variant == SHADER_VARIANT_Y_XUXV) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.yz = texture2D(tex1, v_texcoord).ga;

	} else if (c_variant == SHADER_VARIANT_XYUV) {
		yuva.xyz = texture2D(tex, v_texcoord).bgr;

	} else {
		/* Never reached, bad variant value. */
		return vec4(1.0, 0.3, 1.0, 1.0);
	}

	return yuva2rgba(yuva);
}

/*
 * Sample a 1D LUT which is a single row of a 2D texture. The 2D texture has
 * four rows so that the centers of texels have precise y-coordinates.
 *
 * Texture coordinates go from 0.0 to 1.0 corresponding to texture edges.
 * When we do LUT look-ups with linear filtering, the correct range to sample
 * from is not from edge to edge, but center of first texel to center of last
 * texel. This follows because with LUTs, you have the exact end points given,
 * you never extrapolate but only interpolate.
 * The scale and offset are precomputed to achieve this mapping.
 */
float
sample_lut_1d(HIGHPRECISION sampler2D lut, vec2 scale_offset,
	      float x, compile_const int row)
{
	float tx = x * scale_offset.s + scale_offset.t;
	float ty = (float(row) + 0.5) / 4.0;

	return texture2D(lut, vec2(tx, ty)).x;
}

vec3
sample_lut_3x1d(HIGHPRECISION sampler2D lut, vec2 scale_offset, vec3 color)
{
	return vec3(sample_lut_1d(lut, scale_offset, color.r, 0),
		    sample_lut_1d(lut, scale_offset, color.g, 1),
		    sample_lut_1d(lut, scale_offset, color.b, 2));
}

vec3
lut_texcoord(vec3 pos, vec2 scale_offset)
{
	return pos * scale_offset.s + scale_offset.t;
}

float
linpow(float x, float g, float a, float b, float c, float d)
{
	/* See WESTON_COLOR_CURVE_TYPE_LINPOW for details about LINPOW. */

	if (x >= d)
		return pow((a * x) + b, g);

	return c * x;
}

float
powlin(float x, float g, float a, float b, float c, float d)
{
	/* See WESTON_COLOR_CURVE_TYPE_POWLIN for details about POWLIN. */

	if (x >= d)
		return a * pow(x, g) + b;

	return c * x;
}

float
sample_linpow(float params[MAX_CURVESET_PARAMS], bool must_clamp,
	      float x, compile_const int color_channel)
{
	float g, a, b, c, d;

	/*
	 * For each color channel we have MAX_CURVE_PARAMS parameters.
	 * The parameters for the three curves are stored in RGB order.
	 */
	g = params[0 + color_channel * MAX_CURVE_PARAMS];
	a = params[1 + color_channel * MAX_CURVE_PARAMS];
	b = params[2 + color_channel * MAX_CURVE_PARAMS];
	c = params[3 + color_channel * MAX_CURVE_PARAMS];
	d = params[4 + color_channel * MAX_CURVE_PARAMS];

	if (must_clamp)
		x = clamp(x, 0.0, 1.0);

	/* We use mirroring for negative input values. */
	if (x < 0.0)
		return -linpow(-x, g, a, b, c, d);

	return linpow(x, g, a, b, c, d);
}

vec3
sample_linpow_vec3(float params[MAX_CURVESET_PARAMS], bool must_clamp,
		   vec3 color)
{
	return vec3(sample_linpow(params, must_clamp, color.r, 0),
		    sample_linpow(params, must_clamp, color.g, 1),
		    sample_linpow(params, must_clamp, color.b, 2));
}

float
sample_color_pre_curve_powlin(float x, compile_const int color_channel)
{
	float g, a, b, c, d;

	/* For each color channel we have 10 parameters. The params are
	 * linearized in an array of size 30, in RGB order. */
	g = color_pre_curve_params[0 + (color_channel * 10)];
	a = color_pre_curve_params[1 + (color_channel * 10)];
	b = color_pre_curve_params[2 + (color_channel * 10)];
	c = color_pre_curve_params[3 + (color_channel * 10)];
	d = color_pre_curve_params[4 + (color_channel * 10)];

	if (color_pre_curve_clamped_input)
		x = clamp(x, 0.0, 1.0);

	/* We use mirroring for negative input values. */
	if (x < 0.0)
		return -powlin(-x, g, a, b, c, d);

	return powlin(x, g, a, b, c, d);
}

float
sample_color_post_curve_powlin(float x, compile_const int color_channel)
{
	float g, a, b, c, d;

	/* For each color channel we have 10 parameters. The params are
	 * linearized in an array of size 30, in RGB order. */
	g = color_post_curve_params[0 + (color_channel * 10)];
	a = color_post_curve_params[1 + (color_channel * 10)];
	b = color_post_curve_params[2 + (color_channel * 10)];
	c = color_post_curve_params[3 + (color_channel * 10)];
	d = color_post_curve_params[4 + (color_channel * 10)];

	if (color_post_curve_clamped_input)
		x = clamp(x, 0.0, 1.0);

	/* We use mirroring for negative input values. */
	if (x < 0.0)
		return -powlin(-x, g, a, b, c, d);

	return powlin(x, g, a, b, c, d);
}

vec3
color_pre_curve(vec3 color)
{
	vec3 ret;

	if (c_color_pre_curve == SHADER_COLOR_CURVE_IDENTITY) {
		return color;
	} else if (c_color_pre_curve == SHADER_COLOR_CURVE_LUT_3x1D) {
		return sample_lut_3x1d(color_pre_curve_lut_2d,
				       color_pre_curve_lut_scale_offset,
				       color);
	} else if (c_color_pre_curve == SHADER_COLOR_CURVE_LINPOW) {
		return sample_linpow_vec3(color_pre_curve_params,
					  color_pre_curve_clamped_input,
					  color);
	} else if (c_color_pre_curve == SHADER_COLOR_CURVE_POWLIN) {
		ret.r = sample_color_pre_curve_powlin(color.r, 0);
		ret.g = sample_color_pre_curve_powlin(color.g, 1);
		ret.b = sample_color_pre_curve_powlin(color.b, 2);
		return ret;
	} else {
		/* Never reached, bad c_color_pre_curve. */
		return vec3(1.0, 0.3, 1.0);
	}
}

vec3
sample_color_mapping_lut_3d(vec3 color)
{
	vec3 pos, ret = vec3(0.0, 0.0, 0.0);
#if DEF_COLOR_MAPPING == SHADER_COLOR_MAPPING_3DLUT
	pos = lut_texcoord(color, color_mapping_lut_scale_offset);
	ret = texture3D(color_mapping_lut_3d, pos).rgb;
#endif
	return ret;
}

vec3
color_mapping(vec3 color)
{
	if (c_color_mapping == SHADER_COLOR_MAPPING_IDENTITY)
		return color;
	else if (c_color_mapping == SHADER_COLOR_MAPPING_3DLUT)
		return sample_color_mapping_lut_3d(color);
	else if (c_color_mapping == SHADER_COLOR_MAPPING_MATRIX)
		return color_mapping_matrix * color.rgb;
	else /* Never reached, bad c_color_mapping. */
		return vec3(1.0, 0.3, 1.0);
}

vec3
color_post_curve(vec3 color)
{
	vec3 ret;

	if (c_color_post_curve == SHADER_COLOR_CURVE_IDENTITY) {
		return color;
	} else if (c_color_post_curve == SHADER_COLOR_CURVE_LUT_3x1D) {
		return sample_lut_3x1d(color_post_curve_lut_2d,
				       color_post_curve_lut_scale_offset,
				       color);
	} else if (c_color_post_curve == SHADER_COLOR_CURVE_LINPOW) {
		return sample_linpow_vec3(color_post_curve_params,
					  color_post_curve_clamped_input,
					  color);
	} else if (c_color_post_curve == SHADER_COLOR_CURVE_POWLIN) {
		ret.r = sample_color_post_curve_powlin(color.r, 0);
		ret.g = sample_color_post_curve_powlin(color.g, 1);
		ret.b = sample_color_post_curve_powlin(color.b, 2);
		return ret;
	} else {
		/* Never reached, bad c_color_post_curve. */
		return vec3(1.0, 0.3, 1.0);
	}
}

vec4
color_pipeline(vec4 color)
{
	/* Ensure straight alpha */
	if (c_input_is_premult) {
		if (color.a == 0.0)
			color.rgb = vec3(0, 0, 0);
		else
			color.rgb *= 1.0 / color.a;
	}

	color.rgb = color_pre_curve(color.rgb);
	color.rgb = color_mapping(color.rgb);
	color.rgb = color_post_curve(color.rgb);

	return color;
}

void
main()
{
	vec4 color;

	/* Electrical (non-linear) RGBA values, may be premult or not */
	color = sample_input_texture();

	if (c_need_color_pipeline)
		color = color_pipeline(color); /* Produces straight alpha */

	/* Ensure pre-multiplied for blending */
	if (!c_input_is_premult || c_need_color_pipeline)
		color.rgb *= color.a;

	color *= view_alpha;

	if (c_green_tint)
		color = vec4(0.0, 0.3, 0.0, 0.2) + color * 0.8;

	gl_FragColor = color;
}
