#include "glCanvas.h"

#include "raytracer.h"
#include "material.h"
#include "argparser.h"
#include "raytree.h"
#include "utils.h"
#include "mesh.h"
#include "face.h"
#include "primitive.h"
#include "photon_mapping.h"

// ===========================================================================
// casts a single ray through the scene geometry and finds the closest hit
bool RayTracer::CastRay(const Ray &ray, Hit &h,
		bool use_rasterized_patches) const {
	bool answer = false;

	// intersect each of the quads
	for (int i = 0; i < mesh->numOriginalQuads(); i++) {
		Face *f = mesh->getOriginalQuad(i);
		Hit temphit;
		if (f->intersect(ray, temphit, args->intersect_backfacing)) {
			answer = true;
			if (temphit.getT() < h.getT())
				h = temphit;
		}
	}

	// intersect each of the primitives (either the patches, or the original primitives)
	if (use_rasterized_patches) {
		for (int i = 0; i < mesh->numRasterizedPrimitiveFaces(); i++) {
			Face *f = mesh->getRasterizedPrimitiveFace(i);
			Hit temphit;
			if (f->intersect(ray, temphit, args->intersect_backfacing)) {
				answer = true;
				if (temphit.getT() < h.getT())
					h = temphit;
			}
		}
	} else {
		int num_primitives = mesh->numPrimitives();
		for (int i = 0; i < num_primitives; i++) {
			Hit temphit;
			if (mesh->getPrimitive(i)->intersect(ray, temphit)) {
				answer = true;
				if (temphit.getT() < h.getT())
					h = temphit;
			}
		}
	}
	return answer;
}

// ===========================================================================
// does the recursive (shadow rays & recursive rays) work
glm::vec3 RayTracer::TraceRay(Ray &ray, Hit &hit, int bounce_count) const {

	// First cast a ray and see if we hit anything.
	hit = Hit();
	bool intersect = CastRay(ray, hit, false);

	// if there is no intersection, simply return the background color
	if (intersect == false) {
		return glm::vec3(srgb_to_linear(mesh->background_color.r),
				srgb_to_linear(mesh->background_color.g),
				srgb_to_linear(mesh->background_color.b));
	}

	// otherwise decide what to do based on the material
	Material *m = hit.getMaterial();
	assert(m != NULL);

	// rays coming from the light source are set to white, don't bother to ray trace further.
	if (glm::length(m->getEmittedColor()) > 0.001) {
		return glm::vec3(1, 1, 1);
	}

	glm::vec3 normal = hit.getNormal();
	glm::vec3 point = ray.pointAtParameter(hit.getT());
	glm::vec3 answer;

	// ----------------------------------------------
	//  start with the indirect light (ambient light)
	glm::vec3 diffuse_color = m->getDiffuseColor(hit.get_s(), hit.get_t());
	if (args->gather_indirect) {
		// photon mapping for more accurate indirect light
		answer = diffuse_color
				* (photon_mapping->GatherIndirect(point, normal,
						ray.getDirection()) + args->ambient_light);
	} else {
		// the usual ray tracing hack for indirect light
		answer = diffuse_color * args->ambient_light;
	}

	// ----------------------------------------------
	// add contributions from each light that is not in shadow
	int num_lights = mesh->getLights().size();
	for (int i = 0; i < num_lights; i++) {

		Face *f = mesh->getLights()[i];
		glm::vec3 lightColor = f->getMaterial()->getEmittedColor()
				* f->getArea();
		glm::vec3 myLightColor;
		glm::vec3 lightCentroid = f->computeCentroid();
		glm::vec3 dirToLightCentroid = glm::normalize(lightCentroid - point);

		// ===========================================
		// ASSIGNMENT:  ADD SHADOW & SOFT SHADOW LOGIC
		// ===========================================

		float distToLightCentroid = glm::length(lightCentroid - point);
		myLightColor = lightColor
				/ float(M_PI * distToLightCentroid * distToLightCentroid);

		// add the lighting contribution from this particular light at this point
		// (fix this to check for blockers between the light & this surface
		if (args->num_shadow_samples == 1) {
			Ray tempray(ray.pointAtParameter(hit.getT() * .9999f),
					dirToLightCentroid);
			Hit temphit;
			CastRay(tempray, temphit, false);
			RayTree::AddShadowSegment(tempray, 0.0f, temphit.getT());
			if (glm::length(temphit.getMaterial()->getEmittedColor())
					> 0.001f) {
				answer += m->Shade(ray, hit, dirToLightCentroid, myLightColor,
						args);
			}
		} else {
			for (int count = 0; count < args->num_shadow_samples; count++) {
				glm::vec3 dirToRandPoint = glm::normalize(
						f->RandomPoint() - point);
				Ray tempray(ray.pointAtParameter(hit.getT() * .9999f),
						dirToRandPoint);
				Hit temphit;
				CastRay(tempray, temphit, false);
				RayTree::AddShadowSegment(tempray, 0.0f, temphit.getT());
				if (!temphit.getMaterial()) continue;
				if (glm::length(temphit.getMaterial()->getEmittedColor())
						> 0.001f) {
					answer += m->Shade(ray, hit, dirToLightCentroid,
							myLightColor, args)/(float)(args->num_shadow_samples);
				}
			}
		}

	}

	// ----------------------------------------------
	// add contribution from reflection, if the surface is shiny
	glm::vec3 reflectiveColor = m->getReflectiveColor();

	if (bounce_count > 0) {

		glm::vec3 vel = ray.getDirection();
		glm::vec3 newvel = vel - 2 * (glm::dot(vel, normal)) * normal;
		glm::vec3 neworg = ray.pointAtParameter(hit.getT() * .9999f);
		Ray tempray(neworg, newvel);
		Hit temphit;
		answer += reflectiveColor
				* TraceRay(tempray, temphit, bounce_count - 1);
		RayTree::AddReflectedSegment(tempray, 0.0f, temphit.getT());
	}
	// =================================
	// ASSIGNMENT:  ADD REFLECTIVE LOGIC
	// =================================

	return answer;

}

void RayTracer::initializeVBOs() {
	glGenBuffers(1, &pixels_a_VBO);
	glGenBuffers(1, &pixels_b_VBO);
	glGenBuffers(1, &pixels_indices_a_VBO);
	glGenBuffers(1, &pixels_indices_b_VBO);
	render_to_a = true;
}

void RayTracer::resetVBOs() {

	pixels_a.clear();
	pixels_b.clear();

	pixels_indices_a.clear();
	pixels_indices_b.clear();

	render_to_a = true;
}

void RayTracer::setupVBOs() {

	glBindBuffer(GL_ARRAY_BUFFER, pixels_a_VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(VBOPosNormalColor) * pixels_a.size(),
			&pixels_a[0], GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, pixels_b_VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(VBOPosNormalColor) * pixels_b.size(),
			&pixels_b[0], GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pixels_indices_a_VBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
			sizeof(VBOIndexedTri) * pixels_indices_a.size(),
			&pixels_indices_a[0], GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pixels_indices_b_VBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
			sizeof(VBOIndexedTri) * pixels_indices_b.size(),
			&pixels_indices_b[0], GL_STATIC_DRAW);

}

void RayTracer::drawVBOs() {
	// turn off lighting
	glUniform1i(GLCanvas::colormodeID, 0);
	// turn off depth buffer
	glDisable(GL_DEPTH_TEST);

	if (render_to_a) {
		drawVBOs_b();
		drawVBOs_a();
	} else {
		drawVBOs_a();
		drawVBOs_b();
	}

	glEnable(GL_DEPTH_TEST);
}

void RayTracer::drawVBOs_a() {
	if (pixels_a.size() == 0)
		return;
	glBindBuffer(GL_ARRAY_BUFFER, pixels_a_VBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pixels_indices_a_VBO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VBOPosNormalColor),
			(void*) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VBOPosNormalColor),
			(void*) sizeof(glm::vec3));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(VBOPosNormalColor),
			(void*) (sizeof(glm::vec3) * 2));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(VBOPosNormalColor),
			(void*) (sizeof(glm::vec3) * 2 + sizeof(glm::vec4)));
	glDrawElements(GL_TRIANGLES, (int) pixels_indices_a.size() * 3,
	GL_UNSIGNED_INT, 0);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(3);
}

void RayTracer::drawVBOs_b() {
	if (pixels_b.size() == 0)
		return;
	glBindBuffer(GL_ARRAY_BUFFER, pixels_b_VBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pixels_indices_b_VBO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VBOPosNormalColor),
			(void*) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VBOPosNormalColor),
			(void*) sizeof(glm::vec3));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(VBOPosNormalColor),
			(void*) (sizeof(glm::vec3) * 2));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(VBOPosNormalColor),
			(void*) (sizeof(glm::vec3) * 2 + sizeof(glm::vec4)));
	glDrawElements(GL_TRIANGLES, (int) pixels_indices_b.size() * 3,
	GL_UNSIGNED_INT, 0);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(3);
}

void RayTracer::cleanupVBOs() {
	glDeleteBuffers(1, &pixels_a_VBO);
	glDeleteBuffers(1, &pixels_b_VBO);
}
