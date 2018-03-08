/***************************************************************************
 * Copyright 1998-2018 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <limits>
#include <algorithm>
#include <exception>

#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>

#include "slg/film/film.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Film image pipeline related methods
//------------------------------------------------------------------------------

void Film::SetImagePipelines(ImagePipeline *newImagePiepeline) {
	BOOST_FOREACH(ImagePipeline *ip, imagePipelines)
		delete ip;

	if (newImagePiepeline) {
		imagePipelines.resize(1);
		imagePipelines[0] = newImagePiepeline;
	} else
		imagePipelines.resize(0);
}

void Film::SetImagePipelines(std::vector<ImagePipeline *> &newImagePiepelines) {
	BOOST_FOREACH(ImagePipeline *ip, imagePipelines)
		delete ip;

	imagePipelines = newImagePiepelines;
}

void Film::MergeSampleBuffers(const u_int index) {
	Spectrum *p = (Spectrum *)channel_IMAGEPIPELINEs[index]->GetPixels();
	
	channel_FRAMEBUFFER_MASK->Clear();

	// Merge RADIANCE_PER_PIXEL_NORMALIZED and RADIANCE_PER_SCREEN_NORMALIZED buffers

	if (HasChannel(RADIANCE_PER_PIXEL_NORMALIZED)) {
		for (u_int i = 0; i < radianceGroupCount; ++i) {
			if (radianceChannelScales[i].enabled) {
				#pragma omp parallel for
				for (
						// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
						unsigned
#endif
						int j = 0; j < pixelCount; ++j) {
					const float *sp = channel_RADIANCE_PER_PIXEL_NORMALIZEDs[i]->GetPixel(j);

					if (sp[3] > 0.f) {
						Spectrum s(sp);
						s /= sp[3];
						s = radianceChannelScales[i].Scale(s);

						u_int *fbMask = channel_FRAMEBUFFER_MASK->GetPixel(j);
						if (*fbMask)
							p[j] += s;
						else
							p[j] = s;
						*fbMask = 1;
					}
				}
			}
		}
	}

	if (HasChannel(RADIANCE_PER_SCREEN_NORMALIZED)) {
		const float factor = pixelCount / statsTotalSampleCount;

		for (u_int i = 0; i < radianceGroupCount; ++i) {
			if (radianceChannelScales[i].enabled) {
				#pragma omp parallel for
				for (
						// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
						unsigned
#endif
				int j = 0; j < pixelCount; ++j) {
					Spectrum s(channel_RADIANCE_PER_SCREEN_NORMALIZEDs[i]->GetPixel(j));

					if (!s.Black()) {
						s = factor * radianceChannelScales[i].Scale(s);

						u_int *fbMask = channel_FRAMEBUFFER_MASK->GetPixel(j);
						if (*fbMask)
							p[j] += s;
						else
							p[j] = s;
						*fbMask = 1;
					}
				}
			}
		}
	}

	if (!enabledOverlappedScreenBufferUpdate) {
		for (u_int i = 0; i < pixelCount; ++i) {
			if (!(*(channel_FRAMEBUFFER_MASK->GetPixel(i))))
				p[i] = Spectrum();
		}
	}
}

void Film::ExecuteImagePipeline(const u_int index) {
	if ((!HasChannel(RADIANCE_PER_PIXEL_NORMALIZED) && !HasChannel(RADIANCE_PER_SCREEN_NORMALIZED)) ||
			!HasChannel(IMAGEPIPELINE)) {
		// Nothing to do
		return;
	}

#if !defined(LUXRAYS_DISABLE_OPENCL)
	// Initialize OpenCL device
	if (oclEnable && !ctx) {
		CreateOCLContext();

		if (oclIntersectionDevice) {
			AllocateOCLBuffers();
			CompileOCLKernels();
		}
	}
#endif

	// Merge all buffers
	//const double t1 = WallClockTime();
#if !defined(LUXRAYS_DISABLE_OPENCL)
	if (oclEnable && oclIntersectionDevice)
		MergeSampleBuffersOCL(index);
	else
		MergeSampleBuffers(index);
#else
	MergeSampleBuffers(index);
#endif
	//const double t2 = WallClockTime();
	//SLG_LOG("MergeSampleBuffers time: " << int((t2 - t1) * 1000.0) << "ms");

	// Apply the image pipeline
	//const double p1 = WallClockTime();
#if !defined(LUXRAYS_DISABLE_OPENCL)
	// Transfer all buffers to OpenCL device memory
	if (oclEnable && imagePipelines[index]->CanUseOpenCL())
		WriteAllOCLBuffers();
#endif

	imagePipelines[index]->Apply(*this, index);
	//const double p2 = WallClockTime();
	//SLG_LOG("Image pipeline " << index << " time: " << int((p2 - p1) * 1000.0) << "ms");
}
