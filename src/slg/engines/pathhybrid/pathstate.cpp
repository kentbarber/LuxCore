/***************************************************************************
 *   Copyright (C) 1998-2013 by authors (see AUTHORS.txt)                  *
 *                                                                         *
 *   This file is part of LuxRays.                                         *
 *                                                                         *
 *   LuxRays is free software; you can redistribute it and/or modify       *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   LuxRays is distributed in the hope that it will be useful,            *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 *                                                                         *
 *   LuxRays website: http://www.luxrender.net                             *
 ***************************************************************************/

#include <algorithm>

#include "slg/slg.h"
#include "slg/renderconfig.h"
#include "slg/engines/pathhybrid/pathhybrid.h"

#include "luxrays/core/intersectiondevice.h"

using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// PathState
//------------------------------------------------------------------------------

const u_int sampleBootSize = 4;
const u_int sampleStepSize = 9;

PathHybridState::PathHybridState(PathHybridRenderThread *renderThread,
		Film *film, luxrays::RandomGenerator *rndGen) : HybridRenderState(renderThread, film, rndGen), sampleResults(1) {
	PathHybridRenderEngine *renderEngine = (PathHybridRenderEngine *)renderThread->renderEngine;

	sampleResults[0].type = PER_PIXEL_NORMALIZED;

	// Setup the sampler
	const u_int sampleSize = sampleBootSize + renderEngine->maxPathDepth * sampleStepSize;
	sampler->RequestSamples(sampleSize);

	Init(renderThread);
}

void PathHybridState::Init(const PathHybridRenderThread *thread) {
	PathHybridRenderEngine *renderEngine = (PathHybridRenderEngine *)thread->renderEngine;
	Scene *scene = renderEngine->renderConfig->scene;

	depth = 1;
	lastPdfW = 1.f;
	throuput = Spectrum(1.f);

	directLightRadiance = Spectrum();

	// Initialize eye ray
	PerspectiveCamera *camera = scene->camera;
	Film *film = thread->threadFilm;
	const u_int filmWidth = film->GetWidth();
	const u_int filmHeight = film->GetHeight();

	sampleResults[0].screenX = std::min(sampler->GetSample(0) * filmWidth, (float)(filmWidth - 1));
	sampleResults[0].screenY = std::min(sampler->GetSample(1) * filmHeight, (float)(filmHeight - 1));
	camera->GenerateRay(sampleResults[0].screenX, sampleResults[0].screenY, &nextPathVertexRay,
		sampler->GetSample(2), sampler->GetSample(3));

	sampleResults[0].alpha = 1.f;
	sampleResults[0].radiance = Spectrum(0.f);
	lastSpecular = true;
}


void PathHybridState::DirectHitInfiniteLight(const Scene *scene, const Vector &eyeDir) {
	// Infinite light
	float directPdfW;
	if (scene->envLight) {
		const Spectrum envRadiance = scene->envLight->GetRadiance(*scene, -eyeDir, &directPdfW);
		if (!envRadiance.Black()) {
			if(!lastSpecular) {
				// MIS between BSDF sampling and direct light sampling
				sampleResults[0].radiance += throuput * PowerHeuristic(lastPdfW, directPdfW) * envRadiance;
			} else
				sampleResults[0].radiance += throuput * envRadiance;
		}
	}

	// Sun light
	if (scene->sunLight) {
		const Spectrum sunRadiance = scene->sunLight->GetRadiance(*scene, -eyeDir, &directPdfW);
		if (!sunRadiance.Black()) {
			if(!lastSpecular) {
				// MIS between BSDF sampling and direct light sampling
				sampleResults[0].radiance += throuput * PowerHeuristic(lastPdfW, directPdfW) * sunRadiance;
			} else
				sampleResults[0].radiance += throuput * sunRadiance;
		}
	}
}

void PathHybridState::DirectHitFiniteLight(const Scene *scene, const float distance, const BSDF &bsdf) {
	float directPdfA;
	const Spectrum emittedRadiance = bsdf.GetEmittedRadiance(&directPdfA);

	if (!emittedRadiance.Black()) {
		float weight;
		if (!lastSpecular) {
			const float lightPickProb = scene->PickLightPdf();
			const float directPdfW = PdfAtoW(directPdfA, distance,
				AbsDot(bsdf.hitPoint.fixedDir, bsdf.hitPoint.shadeN));

			// MIS between BSDF sampling and direct light sampling
			weight = PowerHeuristic(lastPdfW, directPdfW * lightPickProb);
		} else
			weight = 1.f;

		sampleResults[0].radiance +=  throuput * weight * emittedRadiance;
	}
}

void PathHybridState::DirectLightSampling(const PathHybridRenderThread *renderThread,
		const float u0, const float u1, const float u2,
		const float u3, const BSDF &bsdf) {
	if (!bsdf.IsDelta()) {
		PathHybridRenderEngine *renderEngine = (PathHybridRenderEngine *)renderThread->renderEngine;
		Scene *scene = renderEngine->renderConfig->scene;

		// Pick a light source to sample
		float lightPickPdf;
		const LightSource *light = scene->SampleAllLights(u0, &lightPickPdf);

		Vector lightRayDir;
		float distance, directPdfW;
		Spectrum lightRadiance = light->Illuminate(*scene, bsdf.hitPoint.p,
				u1, u2, u3, &lightRayDir, &distance, &directPdfW);

		if (!lightRadiance.Black()) {
			BSDFEvent event;
			float bsdfPdfW;
			Spectrum bsdfEval = bsdf.Evaluate(lightRayDir, &event, &bsdfPdfW);

			if (!bsdfEval.Black()) {
				const float epsilon = Max(MachineEpsilon::E(bsdf.hitPoint.p), MachineEpsilon::E(distance));
				directLightRay = Ray(bsdf.hitPoint.p, lightRayDir,
						epsilon, distance - epsilon);

				const float cosThetaToLight = AbsDot(lightRayDir, bsdf.hitPoint.shadeN);
				const float directLightSamplingPdfW = directPdfW * lightPickPdf;
				const float factor = cosThetaToLight / directLightSamplingPdfW;

				if (depth >= renderEngine->rrDepth) {
					// Russian Roulette
					bsdfPdfW *= RenderEngine::RussianRouletteProb(bsdfEval, renderEngine->rrImportanceCap);
				}

				// MIS between direct light sampling and BSDF sampling
				const float weight = PowerHeuristic(directLightSamplingPdfW, bsdfPdfW);

				directLightRadiance = (weight * factor) * throuput * lightRadiance * bsdfEval;
			} else
				directLightRadiance = Spectrum();
		} else
			directLightRadiance = Spectrum();
	} else
		directLightRadiance = Spectrum();
}

bool PathHybridState::FinalizeRay(const PathHybridRenderThread *renderThread,
		const Ray *ray, const RayHit *rayHit, BSDF *bsdf, const float u0, Spectrum *radiance) {
	if (rayHit->Miss())
		return true;
	else {
		PathHybridRenderEngine *renderEngine = (PathHybridRenderEngine *)renderThread->renderEngine;
		Scene *scene = renderEngine->renderConfig->scene;

		// I have to check if it is an hit over a pass-through point
		bsdf->Init(false, *scene, *ray, *rayHit, u0);

		// Check if it is pass-through point
		Spectrum t = bsdf->GetPassThroughTransparency();
		if (!t.Black()) {
			*radiance *= t;

			// It is a pass-through material, continue to trace the ray. I do
			// this on the CPU.

			Ray newRay(*ray);
			newRay.mint = rayHit->t + MachineEpsilon::E(rayHit->t);
			RayHit newRayHit;			
			Spectrum connectionThroughput;
			if (scene->Intersect(renderThread->device, false, u0, &newRay, &newRayHit,
					bsdf, &connectionThroughput)) {
				// Something was hit
				return false;
			} else {
				*radiance *= connectionThroughput;
				return true;
			}
		} else
			return false;
	}
}

void PathHybridState::SplatSample(const PathHybridRenderThread *renderThread) {
	sampler->NextSample(sampleResults);

	Init(renderThread);
}

void PathHybridState::GenerateRays(HybridRenderThread *renderThread) {
	PathHybridRenderThread *thread = (PathHybridRenderThread *)renderThread;

	if (!directLightRadiance.Black())
		thread->PushRay(directLightRay);
	thread->PushRay(nextPathVertexRay);
}

double PathHybridState::CollectResults(HybridRenderThread *renderThread) {
	PathHybridRenderThread *thread = (PathHybridRenderThread *)renderThread;
	PathHybridRenderEngine *renderEngine = (PathHybridRenderEngine *)thread->renderEngine;
	Scene *scene = renderEngine->renderConfig->scene;

	// Was a shadow ray traced ?
	if (!directLightRadiance.Black()) {
		// It was, check the result
		const Ray *shadowRay;
		const RayHit *shadowRayHit;
		thread->PopRay(&shadowRay, &shadowRayHit);

		BSDF bsdf;
		// "(depth - 1 - 1)" is used because it is a shadow ray of previous path vertex
		const unsigned int sampleOffset = sampleBootSize + (depth - 1 - 1) * sampleStepSize;
		const bool miss = FinalizeRay(thread, shadowRay, shadowRayHit, &bsdf,
				sampler->GetSample(sampleOffset + 5), &directLightRadiance);
		if (miss) {
			// The light source is visible, add the direct light sampling component
			sampleResults[0].radiance += directLightRadiance;
		}
	}

	// The next path vertex ray
	const Ray *ray;
	const RayHit *rayHit;
	thread->PopRay(&ray, &rayHit);

	BSDF bsdf;
	const unsigned int sampleOffset = sampleBootSize + (depth - 1) * sampleStepSize;
	const bool miss = FinalizeRay(thread, ray, rayHit, &bsdf, sampler->GetSample(sampleOffset),
			&throuput);

	if (miss) {
		// Nothing was hit, look for infinitelight
		DirectHitInfiniteLight(scene, ray->d);

		if (depth == 1)
			sampleResults[0].alpha = 0.f;

		SplatSample(thread);
		return 1.0;
	} else {
		// Something was hit

		// Check if a light source was hit
		if (bsdf.IsLightSource())
			DirectHitFiniteLight(scene, rayHit->t, bsdf);

		// Direct light sampling (initialize directLightRay)
		DirectLightSampling(thread,
				sampler->GetSample(sampleOffset + 1),
				sampler->GetSample(sampleOffset + 2),
				sampler->GetSample(sampleOffset + 3),
				sampler->GetSample(sampleOffset + 4),
				bsdf);

		// Build the next vertex path ray
		Vector sampledDir;
		BSDFEvent event;
		float cosSampledDir;
		const Spectrum bsdfSample = bsdf.Sample(&sampledDir,
				sampler->GetSample(sampleOffset + 6),
				sampler->GetSample(sampleOffset + 7),
				&lastPdfW, &cosSampledDir, &event);
		if (bsdfSample.Black()) {
			SplatSample(thread);
			return 1.0;
		} else {
			lastSpecular = ((event & SPECULAR) != 0);

			if ((depth >= renderEngine->rrDepth) && !lastSpecular) {
				// Russian Roulette
				const float prob = RenderEngine::RussianRouletteProb(bsdfSample, renderEngine->rrImportanceCap);
				if (sampler->GetSample(sampleOffset + 8) < prob)
					lastPdfW *= prob;
				else {
					SplatSample(thread);
					return 1.0;
				}
			}

			++depth;

			// Check if I have reached the max. path depth
			if (depth > renderEngine->maxPathDepth) {
				SplatSample(thread);
				return 1.0;
			} else {
				throuput *= bsdfSample * (cosSampledDir / lastPdfW);
				assert (!throuput.IsNaN() && !throuput.IsInf());

				nextPathVertexRay = Ray(bsdf.hitPoint.p, sampledDir);
			}
		}
	}

	return 0.0;
}