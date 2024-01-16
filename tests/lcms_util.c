/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 * Copyright 2022 Collabora, Ltd.
 * Copyright (c) 1998-2022 Marti Maria Saguer
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

#include "config.h"

#include <math.h>
#include <lcms2.h>
#include <assert.h>
#include <stdlib.h>

#include "shared/helpers.h"
#include "color_util.h"
#include "lcms_util.h"

static cmsCIExyY wp_d65 = { 0.31271, 0.32902, 1.0 };

/*
 * MPE tone curves can only use LittleCMS parametric curve types 6-8 and not
 * inverses.
 * type 6: Y = (aX + b)^g + c;      params [g, a, b, c]
 * type 7: Y = a log(bX^g + c) + d; params [g, a, b, c, d]
 * type 8: Y = a b^(cX + d) + e;    params [a, b, c, d, e]
 * Additionally, type 0 is sampled segment.
 *
 * cmsCurveSegment.x1 is the breakpoint stored in ICC files, except for the
 * last segment. First segment always begins at -Inf, and last segment always
 * ends at Inf.
 */

static cmsToneCurve *
build_MPE_curve_sRGB(cmsContext ctx)
{
	cmsCurveSegment segments[] = {
		{
			/* Constant zero segment */
			.x0 = -HUGE_VAL,
			.x1 = 0.0,
			.Type = 6,
			.Params = { 1.0, 0.0, 0.0, 0.0 },
		},
		{
			/* Linear segment y = x / 12.92 */
			.x0 = 0.0,
			.x1 = 0.04045,
			.Type = 0,
			.nGridPoints = 2,
			.SampledPoints = (float[]){ 0.0, 0.04045 / 12.92 },
		},
		{
			/* Power segment y = ((x + 0.055) / 1.055)^2.4
			 * which is translated to
			 * y = (1/1.055 * x + 0.055 / 1.055)^2.4 + 0.0
			 */
			.x0 = 0.04045,
			.x1 = 1.0,
			.Type = 6,
			.Params = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 0.0 },
		},
		{
			/* Constant one segment */
			.x0 = 1.0,
			.x1 = HUGE_VAL,
			.Type = 6,
			.Params = { 1.0, 0.0, 0.0, 1.0 },
		}
	};

	return cmsBuildSegmentedToneCurve(ctx, ARRAY_LENGTH(segments), segments);
}

static cmsToneCurve *
build_MPE_curve_sRGB_inv(cmsContext ctx)
{
	cmsCurveSegment segments[] = {
		{
			/* Constant zero segment */
			.x0 = -HUGE_VAL,
			.x1 = 0.0,
			.Type = 6,
			.Params = { 1.0, 0.0, 0.0, 0.0 },
		},
		{
			/* Linear segment y = x * 12.92 */
			.x0 = 0.0,
			.x1 = 0.04045 / 12.92,
			.Type = 0,
			.nGridPoints = 2,
			.SampledPoints = (float[]){ 0.0, 0.04045 },
		},
		{
			/* Power segment y = 1.055 * x^(1/2.4) - 0.055
			 * which is translated to
			 * y = (1.055^2.4 * x + 0.0)^(1/2.4) - 0.055
			 */
			.x0 = 0.04045 / 12.92,
			.x1 = 1.0,
			.Type = 6,
			.Params = { 1.0 / 2.4, pow(1.055, 2.4), 0.0, -0.055 },
		},
		{
			/* Constant one segment */
			.x0 = 1.0,
			.x1 = HUGE_VAL,
			.Type = 6,
			.Params = { 1.0, 0.0, 0.0, 1.0 },
		}
	};

	return cmsBuildSegmentedToneCurve(ctx, ARRAY_LENGTH(segments), segments);
}

static cmsToneCurve *
build_MPE_curve_power(cmsContext ctx, double exponent)
{
	cmsCurveSegment segments[] = {
		{
			/* Constant zero segment */
			.x0 = -HUGE_VAL,
			.x1 = 0.0,
			.Type = 6,
			.Params = { 1.0, 0.0, 0.0, 0.0 },
		},
		{
			/* Power segment y = x^exponent
			 * which is translated to
			 * y = (1.0 * x + 0.0)^exponent + 0.0
			 */
			.x0 = 0.0,
			.x1 = 1.0,
			.Type = 6,
			.Params = { exponent, 1.0, 0.0, 0.0 },
		},
		{
			/* Constant one segment */
			.x0 = 1.0,
			.x1 = HUGE_VAL,
			.Type = 6,
			.Params = { 1.0, 0.0, 0.0, 1.0 },
		}
	};

	return cmsBuildSegmentedToneCurve(ctx, ARRAY_LENGTH(segments), segments);
}

cmsToneCurve *
build_MPE_curve(cmsContext ctx, enum transfer_fn fn)
{
	switch (fn) {
	case TRANSFER_FN_ADOBE_RGB_EOTF:
		return build_MPE_curve_power(ctx, 563.0 / 256.0);
	case TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE:
		return build_MPE_curve_power(ctx, 256.0 / 563.0);
	case TRANSFER_FN_POWER2_4_EOTF:
		return build_MPE_curve_power(ctx, 2.4);
	case TRANSFER_FN_POWER2_4_EOTF_INVERSE:
		return build_MPE_curve_power(ctx, 1.0 / 2.4);
	case TRANSFER_FN_SRGB_EOTF:
		return build_MPE_curve_sRGB(ctx);
	case TRANSFER_FN_SRGB_EOTF_INVERSE:
		return build_MPE_curve_sRGB_inv(ctx);
	default:
		assert(0 && "unimplemented MPE curve");
	}

	return NULL;
}

cmsStage *
build_MPE_curve_stage(cmsContext context_id, enum transfer_fn fn)
{
	cmsToneCurve *c;
	cmsStage *stage;

	c = build_MPE_curve(context_id, fn);
	stage = cmsStageAllocToneCurves(context_id, 3,
					(cmsToneCurve *[3]){ c, c, c });
	assert(stage);
	cmsFreeToneCurve(c);

	return stage;
}

/* This function is taken from LittleCMS, pardon the odd style */
cmsBool
SetTextTags(cmsHPROFILE hProfile, const wchar_t* Description)
{
	cmsMLU *DescriptionMLU, *CopyrightMLU;
	cmsBool  rc = FALSE;
	cmsContext ContextID = cmsGetProfileContextID(hProfile);

	DescriptionMLU  = cmsMLUalloc(ContextID, 1);
	CopyrightMLU    = cmsMLUalloc(ContextID, 1);

	if (DescriptionMLU == NULL || CopyrightMLU == NULL) goto Error;

	if (!cmsMLUsetWide(DescriptionMLU,  "en", "US", Description)) goto Error;
	if (!cmsMLUsetWide(CopyrightMLU,    "en", "US", L"No copyright, use freely")) goto Error;

	if (!cmsWriteTag(hProfile, cmsSigProfileDescriptionTag,  DescriptionMLU)) goto Error;
	if (!cmsWriteTag(hProfile, cmsSigCopyrightTag,           CopyrightMLU)) goto Error;

	rc = TRUE;

Error:

	if (DescriptionMLU)
		cmsMLUfree(DescriptionMLU);
	if (CopyrightMLU)
		cmsMLUfree(CopyrightMLU);
	return rc;
}

static void
test_roundtrip(uint8_t r, uint8_t g, uint8_t b, cmsPipeline *pip,
	       struct rgb_diff_stat *stat)
{
	struct color_float in = { .rgb = { r / 255.0, g / 255.0, b / 255.0 } };
	struct color_float out = {};

	cmsPipelineEvalFloat(in.rgb, out.rgb, pip);
	rgb_diff_stat_update(stat, &in, &out, &in);
}

/*
 * Roundtrip verification tests that converting device -> PCS -> device
 * results in the original color values close enough.
 *
 * This ensures that the two pipelines are probably built correctly, and we
 * do not have problems with unexpected value clamping or with representing
 * (inverse) EOTF curves.
 */
static void
roundtrip_verification(cmsPipeline *DToB, cmsPipeline *BToD, float tolerance)
{
	unsigned r, g, b;
	struct rgb_diff_stat stat = {};
	cmsPipeline *pip;

	pip = cmsPipelineDup(DToB);
	cmsPipelineCat(pip, BToD);

	/*
	 * Inverse-EOTF is known to have precision problems near zero, so
	 * sample near zero densely, the rest can be more sparse to run faster.
	 */
	for (r = 0; r < 256; r += (r < 15) ? 1 : 8) {
		for (g = 0; g < 256; g += (g < 15) ? 1 : 8) {
			for (b = 0; b < 256; b += (b < 15) ? 1 : 8)
				test_roundtrip(r, g, b, pip, &stat);
		}
	}

	cmsPipelineFree(pip);

	rgb_diff_stat_print(&stat, "DToB->BToD roundtrip", 8);
	assert(stat.two_norm.max < tolerance);
}

static cmsInt32Number
sampler_matrix(const float src[], float dst[], void *cargo)
{
	const struct lcmsMAT3 *mat = cargo;
	struct color_float in = { .r = src[0], .g = src[1], .b = src[2] };
	struct color_float cf;
	unsigned i;

	cf = color_float_apply_matrix(mat, in);

	for (i = 0; i < COLOR_CHAN_NUM; i++)
		dst[i] = cf.rgb[i];

	return 1;
}

static cmsStage *
create_cLUT_from_matrix(cmsContext context_id, const struct lcmsMAT3 *mat,
			int dim_size)
{
	cmsStage *cLUT_stage;

	assert(dim_size);

	cLUT_stage = cmsStageAllocCLutFloat(context_id, dim_size, 3, 3, NULL);
	cmsStageSampleCLutFloat(cLUT_stage, sampler_matrix, (void *)mat, 0);

	return cLUT_stage;
}

static void
vcgt_tag_add_to_profile(cmsContext context_id, cmsHPROFILE profile,
			const double vcgt_exponents[COLOR_CHAN_NUM])
{
	cmsToneCurve *vcgt_tag_curves[COLOR_CHAN_NUM];
	unsigned int i;

	if (!should_include_vcgt(vcgt_exponents))
		return;

	for (i = 0; i < COLOR_CHAN_NUM; i++)
		vcgt_tag_curves[i] = cmsBuildGamma(context_id, vcgt_exponents[i]);

	assert(cmsWriteTag(profile, cmsSigVcgtTag, vcgt_tag_curves));

	cmsFreeToneCurveTriple(vcgt_tag_curves);
}

cmsHPROFILE
build_lcms_clut_profile_output(cmsContext context_id,
                               const struct lcms_pipeline *pipeline,
			       const double vcgt_exponents[COLOR_CHAN_NUM],
			       int clut_dim_size, float clut_roundtrip_tolerance)
{
	enum transfer_fn inv_eotf_fn = pipeline->post_fn;
	enum transfer_fn eotf_fn = transfer_fn_invert(inv_eotf_fn);
	cmsHPROFILE hRGB;
	cmsPipeline *DToB0, *BToD0;
	cmsStage *stage;
	cmsStage *stage_inv_eotf;
	cmsStage *stage_eotf;
	struct lcmsMAT3 mat2XYZ_inv;

	lcmsMAT3_invert(&mat2XYZ_inv, &pipeline->mat2XYZ);

	hRGB = cmsCreateProfilePlaceholder(context_id);
	cmsSetProfileVersion(hRGB, 4.3);
	cmsSetDeviceClass(hRGB, cmsSigDisplayClass);
	cmsSetColorSpace(hRGB, cmsSigRgbData);
	cmsSetPCS(hRGB, cmsSigXYZData);
	SetTextTags(hRGB, L"cLut profile");

	stage_eotf = build_MPE_curve_stage(context_id, eotf_fn);
	stage_inv_eotf = build_MPE_curve_stage(context_id, inv_eotf_fn);

	/*
	 * Pipeline from PCS (optical) to device (electrical)
	 */
	BToD0 = cmsPipelineAlloc(context_id, 3, 3);

	stage = create_cLUT_from_matrix(context_id, &mat2XYZ_inv, clut_dim_size);
	cmsPipelineInsertStage(BToD0, cmsAT_END, stage);
	cmsPipelineInsertStage(BToD0, cmsAT_END, cmsStageDup(stage_inv_eotf));

	cmsWriteTag(hRGB, cmsSigBToD0Tag, BToD0);
	cmsLinkTag(hRGB, cmsSigBToD1Tag, cmsSigBToD0Tag);
	cmsLinkTag(hRGB, cmsSigBToD2Tag, cmsSigBToD0Tag);
	cmsLinkTag(hRGB, cmsSigBToD3Tag, cmsSigBToD0Tag);

	/*
	 * Pipeline from device (electrical) to PCS (optical)
	 */
	DToB0 = cmsPipelineAlloc(context_id, 3, 3);

	cmsPipelineInsertStage(DToB0, cmsAT_END, cmsStageDup(stage_eotf));
	stage = create_cLUT_from_matrix(context_id, &pipeline->mat2XYZ, clut_dim_size);
	cmsPipelineInsertStage(DToB0, cmsAT_END, stage);

	cmsWriteTag(hRGB, cmsSigDToB0Tag, DToB0);
	cmsLinkTag(hRGB, cmsSigDToB1Tag, cmsSigDToB0Tag);
	cmsLinkTag(hRGB, cmsSigDToB2Tag, cmsSigDToB0Tag);
	cmsLinkTag(hRGB, cmsSigDToB3Tag, cmsSigDToB0Tag);

	vcgt_tag_add_to_profile(context_id, hRGB, vcgt_exponents);

	roundtrip_verification(DToB0, BToD0, clut_roundtrip_tolerance);

	cmsPipelineFree(BToD0);
	cmsPipelineFree(DToB0);
	cmsStageFree(stage_eotf);
	cmsStageFree(stage_inv_eotf);

	return hRGB;
}

cmsHPROFILE
build_lcms_matrix_shaper_profile_output(cmsContext context_id,
                                        const struct lcms_pipeline *pipeline,
					const double vcgt_exponents[COLOR_CHAN_NUM])
{
	cmsToneCurve *arr_curves[3];
	cmsHPROFILE hRGB;
	int type_inverse_tone_curve;
	double inverse_tone_curve_param[5];

	assert(find_tone_curve_type(pipeline->post_fn, &type_inverse_tone_curve,
				    inverse_tone_curve_param));

	/*
	 * We are creating output profile and therefore we can use the following:
	 * calling semantics:
	 * cmsBuildParametricToneCurve(type_inverse_tone_curve, inverse_tone_curve_param)
	 * The function find_tone_curve_type sets the type of curve positive if it
	 * is tone curve and negative if it is inverse. When we create an ICC
	 * profile we should use a tone curve, the inversion is done by LCMS
	 * when the profile is used for output.
	 */

	arr_curves[0] = arr_curves[1] = arr_curves[2] =
		cmsBuildParametricToneCurve(context_id,
					    (-1) * type_inverse_tone_curve,
					    inverse_tone_curve_param);

	assert(arr_curves[0]);
	hRGB = cmsCreateRGBProfileTHR(context_id, &wp_d65,
				      &pipeline->prim_output, arr_curves);
	assert(hRGB);

	vcgt_tag_add_to_profile(context_id, hRGB, vcgt_exponents);

	cmsFreeToneCurve(arr_curves[0]);
	return hRGB;
}
