#include "BspRenderer.h"
#include "VertexBuffer.h"
#include "primitives.h"
#include "rad.h"
#include "lodepng.h"
#include <algorithm>

BspRenderer::BspRenderer(Bsp* map, ShaderProgram* bspShader, ShaderProgram* colorShader, PointEntRenderer* pointEntRenderer) {
	this->map = map;
	this->bspShader = bspShader;
	this->colorShader = colorShader;
	this->pointEntRenderer = pointEntRenderer;

	loadTextures();
	loadLightmaps();
	preRenderFaces();
	preRenderEnts();
	calcFaceMaths();

	bspShader->bind();

	uint sTexId = glGetUniformLocation(bspShader->ID, "sTex");

	glUniform1i(sTexId, 0);

	for (int s = 0; s < MAXLIGHTMAPS; s++) {
		uint sLightmapTexIds = glGetUniformLocation(bspShader->ID, ("sLightmapTex" + to_string(s)).c_str());

		// assign lightmap texture units (skips the normal texture unit)
		glUniform1i(sLightmapTexIds, s + 1);
	}
}

void BspRenderer::loadTextures() {
	whiteTex = new Texture(1, 1);
	greyTex = new Texture(1, 1);
	redTex = new Texture(1, 1);
	yellowTex = new Texture(1, 1);
	blackTex = new Texture(1, 1);
	
	*((COLOR3*)(whiteTex->data)) = { 255, 255, 255 };
	*((COLOR3*)(redTex->data)) = { 110, 0, 0 };
	*((COLOR3*)(yellowTex->data)) = { 255, 255, 0 };
	*((COLOR3*)(greyTex->data)) = { 64, 64, 64 };
	*((COLOR3*)(blackTex->data)) = { 0, 0, 0 };

	whiteTex->upload();
	redTex->upload();
	yellowTex->upload();
	greyTex->upload();
	blackTex->upload();

	vector<Wad*> wads;
	vector<string> wadNames;
	for (int i = 0; i < map->ents.size(); i++) {
		if (map->ents[i]->keyvalues["classname"] == "worldspawn") {
			wadNames = splitString(map->ents[i]->keyvalues["wad"], ";");

			for (int k = 0; k < wadNames.size(); k++) {
				wadNames[k] = basename(wadNames[k]);
			}
			break;
		}
	}

	vector<string> tryPaths = {
		g_game_path + "/svencoop/",
		g_game_path + "/svencoop_addon/",
		g_game_path + "/svencoop_downloads/",
		g_game_path + "/svencoop_hd/"
	};

	
	for (int i = 0; i < wadNames.size(); i++) {
		string path;
		for (int k = 0; k < tryPaths.size(); k++) {
			string tryPath = tryPaths[k] + wadNames[i];
			if (fileExists(tryPath)) {
				path = tryPath;
				break;
			}
		}

		if (path.empty()) {
			printf("Missing WAD: %s\n", wadNames[i].c_str());
			continue;
		}

		printf("Loading WAD %s\n", path.c_str());
		Wad* wad = new Wad(path);
		wad->readInfo();
		wads.push_back(wad);
	}

	glTextures = new Texture * [map->textureCount];
	for (int i = 0; i < map->textureCount; i++) {
		int32_t texOffset = ((int32_t*)map->textures)[i + 1];
		BSPMIPTEX& tex = *((BSPMIPTEX*)(map->textures + texOffset));

		COLOR3* palette;
		byte* src;
		WADTEX* wadTex = NULL;

		int lastMipSize = (tex.nWidth / 8) * (tex.nHeight / 8);

		if (tex.nOffsets[0] <= 0) {

			bool foundInWad = false;
			for (int k = 0; k < wads.size(); k++) {
				if (wads[k]->hasTexture(tex.szName)) {
					foundInWad = true;

					wadTex = wads[k]->readTexture(tex.szName);
					palette = (COLOR3*)(wadTex->data + wadTex->nOffsets[3] + lastMipSize + 2 - 40);
					src = wadTex->data;

					break;
				}
			}

			if (!foundInWad) {
				glTextures[i] = whiteTex;
				continue;
			}
		}
		else {
			palette = (COLOR3*)(map->textures + texOffset + tex.nOffsets[3] + lastMipSize + 2);
			src = map->textures + texOffset + tex.nOffsets[0];
		}

		COLOR3* imageData = new COLOR3[tex.nWidth * tex.nHeight];
		int sz = tex.nWidth * tex.nHeight;

		for (int k = 0; k < sz; k++) {
			imageData[k] = palette[src[k]];
		}

		if (wadTex) {
			delete wadTex;
		}

		// map->textures + texOffset + tex.nOffsets[0]

		glTextures[i] = new Texture(tex.nWidth, tex.nHeight, imageData);
	}

	for (int i = 0; i < wads.size(); i++) {
		delete wads[i];
	}
}

void BspRenderer::loadLightmaps() {
	vector<LightmapNode*> atlases;
	vector<Texture*> atlasTextures;
	atlases.push_back(new LightmapNode(0, 0, LIGHTMAP_ATLAS_SIZE, LIGHTMAP_ATLAS_SIZE));
	atlasTextures.push_back(new Texture(LIGHTMAP_ATLAS_SIZE, LIGHTMAP_ATLAS_SIZE));
	memset(atlasTextures[0]->data, 0, LIGHTMAP_ATLAS_SIZE * LIGHTMAP_ATLAS_SIZE * sizeof(COLOR3));

	lightmaps = new LightmapInfo[map->faceCount];
	memset(lightmaps, 0, map->faceCount * sizeof(LightmapInfo));

	printf("Calculating lightmaps\n");
	qrad_init_globals(map);

	int lightmapCount = 0;
	int atlasId = 0;
	for (int i = 0; i < map->faceCount; i++) {
		BSPFACE& face = map->faces[i];
		BSPTEXTUREINFO& texinfo = map->texinfos[face.iTextureInfo];

		if (face.nLightmapOffset < 0 || (texinfo.nFlags & TEX_SPECIAL))
			continue;

		int size[2];
		int dummy[2];
		int imins[2];
		int imaxs[2];
		GetFaceLightmapSize(i, size);
		GetFaceExtents(i, imins, imaxs);

		LightmapInfo& info = lightmaps[i];
		info.w = size[0];
		info.h = size[1];
		info.midTexU = (float)(size[0]) / 2.0f;
		info.midTexV = (float)(size[1]) / 2.0f;

		// TODO: float mins/maxs not needed?
		info.midPolyU = (imins[0] + imaxs[0]) * 16 / 2.0f;
		info.midPolyV = (imins[1] + imaxs[1]) * 16 / 2.0f;

		for (int s = 0; s < MAXLIGHTMAPS; s++) {
			if (face.nStyles[s] == 255)
				continue;

			// TODO: Try fitting in earlier atlases before using the latest one
			if (!atlases[atlasId]->insert(info.w, info.h, info.x[s], info.y[s])) {
				atlases.push_back(new LightmapNode(0, 0, LIGHTMAP_ATLAS_SIZE, LIGHTMAP_ATLAS_SIZE));
				atlasTextures.push_back(new Texture(LIGHTMAP_ATLAS_SIZE, LIGHTMAP_ATLAS_SIZE));
				atlasId++;
				memset(atlasTextures[atlasId]->data, 0, LIGHTMAP_ATLAS_SIZE * LIGHTMAP_ATLAS_SIZE * sizeof(COLOR3));

				if (!atlases[atlasId]->insert(info.w, info.h, info.x[s], info.y[s])) {
					printf("Lightmap too big for atlas size!\n");
					continue;
				}
			}

			lightmapCount++;

			info.atlasId[s] = atlasId;

			// copy lightmap data into atlas
			int lightmapSz = info.w * info.h * sizeof(COLOR3);
			COLOR3* lightSrc = (COLOR3*)(map->lightdata + face.nLightmapOffset + s * lightmapSz);
			COLOR3* lightDst = (COLOR3*)(atlasTextures[atlasId]->data);
			for (int y = 0; y < info.h; y++) {
				for (int x = 0; x < info.w; x++) {
					int src = y * info.w + x;
					int dst = (info.y[s] + y) * LIGHTMAP_ATLAS_SIZE + info.x[s] + x;
					lightDst[dst] = lightSrc[src];
				}
			}
		}
	}

	glLightmapTextures = new Texture * [atlasTextures.size()];
	for (int i = 0; i < atlasTextures.size(); i++) {
		delete atlases[i];
		glLightmapTextures[i] = atlasTextures[i];
		glLightmapTextures[i]->upload();
	}

	lodepng_encode24_file("atlas.png", atlasTextures[0]->data, LIGHTMAP_ATLAS_SIZE, LIGHTMAP_ATLAS_SIZE);
	printf("Fit %d lightmaps into %d atlases\n", lightmapCount, atlasId + 1);
}

void BspRenderer::preRenderFaces() {
	renderModels = new RenderModel[map->modelCount];

	for (int m = 0; m < map->modelCount; m++) {
		BSPMODEL& model = map->models[m];
		RenderModel& renderModel = renderModels[m];

		vector<RenderGroup> renderGroups;
		vector<vector<lightmapVert>> renderGroupVerts;
		vector<vector<lightmapVert>> renderGroupWireframeVerts;

		for (int i = 0; i < model.nFaces; i++) {
			int faceIdx = model.iFirstFace + i;
			BSPFACE& face = map->faces[faceIdx];
			BSPTEXTUREINFO& texinfo = map->texinfos[face.iTextureInfo];
			int32_t texOffset = ((int32_t*)map->textures)[texinfo.iMiptex + 1];
			BSPMIPTEX& tex = *((BSPMIPTEX*)(map->textures + texOffset));
			LightmapInfo& lmap = lightmaps[faceIdx];

			lightmapVert* verts = new lightmapVert[face.nEdges];
			int vertCount = face.nEdges;
			Texture* texture = glTextures[texinfo.iMiptex];
			Texture* lightmapAtlas[MAXLIGHTMAPS];

			float tw = 1.0f / (float)tex.nWidth;
			float th = 1.0f / (float)tex.nHeight;

			float lw = (float)lmap.w / (float)LIGHTMAP_ATLAS_SIZE;
			float lh = (float)lmap.h / (float)LIGHTMAP_ATLAS_SIZE;

			bool isSpecial = texinfo.nFlags & TEX_SPECIAL;
			bool hasLighting = face.nStyles[0] != 255 && face.nLightmapOffset >= 0 && !isSpecial;
			for (int s = 0; s < MAXLIGHTMAPS; s++) {
				lightmapAtlas[s] = glLightmapTextures[lmap.atlasId[s]];
			}

			if (isSpecial) {
				lightmapAtlas[0] = whiteTex;
			}

			float opacity = isSpecial ? 0.5f : 1.0f;

			for (int e = 0; e < face.nEdges; e++) {
				int32_t edgeIdx = map->surfedges[face.iFirstEdge + e];
				BSPEDGE& edge = map->edges[abs(edgeIdx)];
				int vertIdx = edgeIdx < 0 ? edge.iVertex[1] : edge.iVertex[0];

				vec3& vert = map->verts[vertIdx];
				verts[e].x = vert.x;
				verts[e].y = vert.z;
				verts[e].z = -vert.y;

				// texture coords
				float fU = dotProduct(texinfo.vS, vert) + texinfo.shiftS;
				float fV = dotProduct(texinfo.vT, vert) + texinfo.shiftT;
				verts[e].u = fU * tw;
				verts[e].v = fV * th;
				verts[e].opacity = isSpecial ? 0.5f : 1.0f;

				// lightmap texture coords
				if (hasLighting) {
					float fLightMapU = lmap.midTexU + (fU - lmap.midPolyU) / 16.0f;
					float fLightMapV = lmap.midTexV + (fV - lmap.midPolyV) / 16.0f;

					float uu = (fLightMapU / (float)lmap.w) * lw;
					float vv = (fLightMapV / (float)lmap.h) * lh;

					float pixelStep = 1.0f / (float)LIGHTMAP_ATLAS_SIZE;

					for (int s = 0; s < MAXLIGHTMAPS; s++) {
						verts[e].luv[s][0] = uu + lmap.x[s] * pixelStep;
						verts[e].luv[s][1] = vv + lmap.y[s] * pixelStep;
					}
				}
				// set lightmap scales
				for (int s = 0; s < MAXLIGHTMAPS; s++) {
					verts[e].luv[s][2] = (hasLighting && face.nStyles[s] != 255) ? 1.0f : 0.0f;
					if (isSpecial && s == 0) {
						verts[e].luv[s][2] = 1.0f;
					}
				}
			}


			// convert TRIANGLE_FAN verts to TRIANGLES so multiple faces can be drawn in a single draw call
			int newCount = face.nEdges + max(0, face.nEdges - 3) * 2;
			int wireframeVertCount = face.nEdges * 2;
			lightmapVert* newVerts = new lightmapVert[newCount];
			lightmapVert* wireframeVerts = new lightmapVert[wireframeVertCount];

			int idx = 0;
			for (int k = 2; k < face.nEdges; k++) {
				newVerts[idx++] = verts[0];
				newVerts[idx++] = verts[k - 1];
				newVerts[idx++] = verts[k];
			}

			idx = 0;
			for (int k = 0; k < face.nEdges; k++) {
				wireframeVerts[idx++] = verts[k];
				wireframeVerts[idx++] = verts[(k+1) % face.nEdges];
			}
			for (int k = 0; k < wireframeVertCount; k++) {
				wireframeVerts[k].luv[0][2] = 1.0f;
				wireframeVerts[k].luv[1][2] = 0.0f;
				wireframeVerts[k].luv[2][2] = 0.0f;
				wireframeVerts[k].luv[3][2] = 0.0f;
				wireframeVerts[k].opacity = 1.0f;
			}

			delete[] verts;
			verts = newVerts;
			vertCount = newCount;

			// add face to a render group (faces that share that same textures and opacity flag)
			bool isTransparent = opacity < 1.0f;
			int groupIdx = -1;
			for (int k = 0; k < renderGroups.size(); k++) {
				if (renderGroups[k].texture == glTextures[texinfo.iMiptex] && renderGroups[k].transparent == isTransparent) {
					bool allMatch = true;
					for (int s = 0; s < MAXLIGHTMAPS; s++) {
						if (renderGroups[k].lightmapAtlas[s] != lightmapAtlas[s]) {
							allMatch = false;
							break;
						}
					}
					if (allMatch) {
						groupIdx = k;
						break;
					}
				}
			}

			if (groupIdx == -1) {
				RenderGroup newGroup = RenderGroup();
				newGroup.vertCount = 0;
				newGroup.verts = NULL;
				newGroup.transparent = isTransparent;
				newGroup.texture = glTextures[texinfo.iMiptex];
				for (int s = 0; s < MAXLIGHTMAPS; s++) {
					newGroup.lightmapAtlas[s] = lightmapAtlas[s];
				}
				renderGroups.push_back(newGroup);
				renderGroupVerts.push_back(vector<lightmapVert>());
				renderGroupWireframeVerts.push_back(vector<lightmapVert>());
				groupIdx = renderGroups.size() - 1;
			}

			for (int k = 0; k < vertCount; k++)
				renderGroupVerts[groupIdx].push_back(verts[k]);
			for (int k = 0; k < wireframeVertCount; k++) {
				renderGroupWireframeVerts[groupIdx].push_back(wireframeVerts[k]);
			}

			delete[] verts;
			delete[] wireframeVerts;
		}

		renderModel.renderGroups = new RenderGroup[renderGroups.size()];
		renderModel.groupCount = renderGroups.size();

		for (int i = 0; i < renderGroups.size(); i++) {
			renderGroups[i].verts = new lightmapVert[renderGroupVerts[i].size()];
			renderGroups[i].vertCount = renderGroupVerts[i].size();
			memcpy(renderGroups[i].verts, &renderGroupVerts[i][0], renderGroups[i].vertCount * sizeof(lightmapVert));

			renderGroups[i].wireframeVerts = new lightmapVert[renderGroupWireframeVerts[i].size()];
			renderGroups[i].wireframeVertCount = renderGroupWireframeVerts[i].size();
			memcpy(renderGroups[i].wireframeVerts, &renderGroupWireframeVerts[i][0], renderGroups[i].wireframeVertCount * sizeof(lightmapVert));

			renderGroups[i].buffer = new VertexBuffer(bspShader, 0);
			renderGroups[i].buffer->addAttribute(TEX_2F, "vTex");
			renderGroups[i].buffer->addAttribute(3, GL_FLOAT, 0, "vLightmapTex0");
			renderGroups[i].buffer->addAttribute(3, GL_FLOAT, 0, "vLightmapTex1");
			renderGroups[i].buffer->addAttribute(3, GL_FLOAT, 0, "vLightmapTex2");
			renderGroups[i].buffer->addAttribute(3, GL_FLOAT, 0, "vLightmapTex3");
			renderGroups[i].buffer->addAttribute(1, GL_FLOAT, 0, "vOpacity");
			renderGroups[i].buffer->addAttribute(POS_3F, "vPosition");
			renderGroups[i].buffer->setData(renderGroups[i].verts, renderGroups[i].vertCount);
			renderGroups[i].buffer->upload();

			renderGroups[i].wireframeBuffer = new VertexBuffer(bspShader, 0);
			renderGroups[i].wireframeBuffer->addAttribute(TEX_2F, "vTex");
			renderGroups[i].wireframeBuffer->addAttribute(3, GL_FLOAT, 0, "vLightmapTex0");
			renderGroups[i].wireframeBuffer->addAttribute(3, GL_FLOAT, 0, "vLightmapTex1");
			renderGroups[i].wireframeBuffer->addAttribute(3, GL_FLOAT, 0, "vLightmapTex2");
			renderGroups[i].wireframeBuffer->addAttribute(3, GL_FLOAT, 0, "vLightmapTex3");
			renderGroups[i].wireframeBuffer->addAttribute(1, GL_FLOAT, 0, "vOpacity");
			renderGroups[i].wireframeBuffer->addAttribute(POS_3F, "vPosition");
			renderGroups[i].wireframeBuffer->setData(renderGroups[i].wireframeVerts, renderGroups[i].wireframeVertCount);
			renderGroups[i].wireframeBuffer->upload();

			renderModel.renderGroups[i] = renderGroups[i];
		}

		printf("Added %d render groups for model %d\n", renderModel.groupCount, m);
	}
}

void BspRenderer::preRenderEnts() {
	renderEnts = new RenderEnt[map->ents.size()];

	for (int i = 0; i < map->ents.size(); i++) {
		Entity* ent = map->ents[i];

		renderEnts[i].modelIdx = ent->getBspModelIdx();
		renderEnts[i].modelMat.loadIdentity();
		renderEnts[i].offset = vec3(0, 0, 0);
		renderEnts[i].pointEntCube = pointEntRenderer->getEntCube(ent);

		if (ent->hasKey("origin")) {
			vec3 origin = Keyvalue("", ent->keyvalues["origin"]).getVector();
			renderEnts[i].modelMat.translate(origin.x, origin.z, -origin.y);
			renderEnts[i].offset = origin;
		}
	}
}

void BspRenderer::calcFaceMaths() {
	faceMaths = new FaceMath[map->faceCount];

	vec3 world_x = vec3(1, 0, 0);
	vec3 world_y = vec3(0, 1, 0);
	vec3 world_z = vec3(0, 0, 1);

	for (int i = 0; i < map->faceCount; i++) {
		FaceMath& faceMath = faceMaths[i];
		BSPFACE& face = map->faces[i];
		BSPPLANE& plane = map->planes[face.iPlane];
		vec3 planeNormal = face.nPlaneSide ? plane.vNormal * -1 : plane.vNormal;
		float fDist = face.nPlaneSide ? -plane.fDist : plane.fDist;

		faceMath.normal = planeNormal;
		faceMath.fdist = fDist;

		faceMath.verts = new vec3[face.nEdges];
		faceMath.vertCount = face.nEdges;

		for (int e = 0; e < face.nEdges; e++) {
			int32_t edgeIdx = map->surfedges[face.iFirstEdge + e];
			BSPEDGE& edge = map->edges[abs(edgeIdx)];
			int vertIdx = edgeIdx < 0 ? edge.iVertex[1] : edge.iVertex[0];
			faceMath.verts[e] = map->verts[vertIdx];
		}

		vec3 plane_x = (faceMath.verts[1] - faceMath.verts[0]).normalize(1.0f);
		vec3 plane_y = crossProduct(planeNormal, plane_x).normalize(1.0f);
		vec3 plane_z = planeNormal;

		faceMath.worldToLocal.loadIdentity();
		faceMath.worldToLocal.m[0 * 4 + 0] = dotProduct(plane_x, world_x);
		faceMath.worldToLocal.m[1 * 4 + 0] = dotProduct(plane_x, world_y);
		faceMath.worldToLocal.m[2 * 4 + 0] = dotProduct(plane_x, world_z);
		faceMath.worldToLocal.m[0 * 4 + 1] = dotProduct(plane_y, world_x);
		faceMath.worldToLocal.m[1 * 4 + 1] = dotProduct(plane_y, world_y);
		faceMath.worldToLocal.m[2 * 4 + 1] = dotProduct(plane_y, world_z);
		faceMath.worldToLocal.m[0 * 4 + 2] = dotProduct(plane_z, world_x);
		faceMath.worldToLocal.m[1 * 4 + 2] = dotProduct(plane_z, world_y);
		faceMath.worldToLocal.m[2 * 4 + 2] = dotProduct(plane_z, world_z);
	}
}

BspRenderer::~BspRenderer() {
	for (int i = 0; i < map->textureCount; i++) {
		delete glTextures[i];
	}
	delete[] glTextures;

	// TODO: more stuff to delete
}

void BspRenderer::render(int highlightEnt) {
	BSPMODEL& world = map->models[0];	

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// draw highlighted ent first so other ent edges don't overlap the highlighted edges
	if (highlightEnt > 0) {
		if (renderEnts[highlightEnt].modelIdx >= 0) {
			bspShader->pushMatrix(MAT_MODEL);
			*bspShader->modelMat = renderEnts[highlightEnt].modelMat;
			bspShader->updateMatrixes();

			drawModel(renderEnts[highlightEnt].modelIdx, false, true, true);
			drawModel(renderEnts[highlightEnt].modelIdx, true, true, true);

			bspShader->popMatrix(MAT_MODEL);
		}
	}

	for (int pass = 0; pass < 2; pass++) {
		bool drawTransparentFaces = pass == 1;

		drawModel(0, drawTransparentFaces, false, false);

		for (int i = 0, sz = map->ents.size(); i < sz; i++) {
			if (renderEnts[i].modelIdx >= 0) {
				bspShader->pushMatrix(MAT_MODEL);
				*bspShader->modelMat = renderEnts[i].modelMat;
				bspShader->updateMatrixes();

				drawModel(renderEnts[i].modelIdx, drawTransparentFaces, i == highlightEnt, false);

				bspShader->popMatrix(MAT_MODEL);
			}
		}

		if ((g_render_flags & RENDER_POINT_ENTS) && pass == 0) {
			drawPointEntities(highlightEnt);
		}
	}
}

void BspRenderer::drawModel(int modelIdx, bool transparent, bool highlight, bool edgesOnly) {

	if (edgesOnly) {
		for (int i = 0; i < renderModels[modelIdx].groupCount; i++) {
			RenderGroup& rgroup = renderModels[modelIdx].renderGroups[i];

			glActiveTexture(GL_TEXTURE0);
			if (highlight)
				yellowTex->bind();
			else
				greyTex->bind();
			glActiveTexture(GL_TEXTURE1);
			whiteTex->bind();

			rgroup.wireframeBuffer->draw(GL_LINES);
		}
		return;
	}

	for (int i = 0; i < renderModels[modelIdx].groupCount; i++) {
		RenderGroup& rgroup = renderModels[modelIdx].renderGroups[i];

		if (rgroup.transparent != transparent)
			continue;

		if (rgroup.transparent) {
			if (modelIdx == 0 && !(g_render_flags & RENDER_SPECIAL)) {
				continue;
			}
			else if (modelIdx != 0 && !(g_render_flags & RENDER_SPECIAL_ENTS)) {
				continue;
			}
		}
		else if (modelIdx != 0 && !(g_render_flags & RENDER_ENTS)) {
			continue;
		}
		
		glActiveTexture(GL_TEXTURE0);
		if (g_render_flags & RENDER_TEXTURES) {
			rgroup.texture->bind();
		}
		else {
			whiteTex->bind();
		}
		

		for (int s = 0; s < MAXLIGHTMAPS; s++) {
			glActiveTexture(GL_TEXTURE1 + s);

			if (highlight) {
				redTex->bind();
			}
			else if (g_render_flags & RENDER_LIGHTMAPS) {
				rgroup.lightmapAtlas[s]->bind();
			}
			else {
				if (s == 0) {
					whiteTex->bind();
				}
				else {
					blackTex->bind();
				}
			}
		}

		rgroup.buffer->draw(GL_TRIANGLES);

		if (highlight || (g_render_flags & RENDER_WIREFRAME)) {
			glActiveTexture(GL_TEXTURE0);
			if (highlight)
				yellowTex->bind();
			else
				greyTex->bind();
			glActiveTexture(GL_TEXTURE1);
			whiteTex->bind();

			rgroup.wireframeBuffer->draw(GL_LINES);
		}
	}
}

void BspRenderer::drawPointEntities(int highlightEnt) {

	colorShader->bind();

	// skip worldspawn
	for (int i = 1, sz = map->ents.size(); i < sz; i++) {
		if (map->ents[i]->isBspModel())
			continue;

		colorShader->pushMatrix(MAT_MODEL);
		*colorShader->modelMat = renderEnts[i].modelMat;
		colorShader->updateMatrixes();

		if (highlightEnt == i) {
			renderEnts[i].pointEntCube->selectBuffer->draw(GL_TRIANGLES);
			renderEnts[i].pointEntCube->wireframeBuffer->draw(GL_LINES);
		}
		else {
			renderEnts[i].pointEntCube->buffer->draw(GL_TRIANGLES);
		}
		
		colorShader->popMatrix(MAT_MODEL);
	}
}

bool BspRenderer::pickPoly(vec3 start, vec3 dir, PickInfo& pickInfo) {
	bool foundBetterPick = false;

	if (pickPoly(start, dir, vec3(0, 0, 0), 0, pickInfo)) {
		pickInfo.entIdx = 0;
		pickInfo.modelIdx = 0;
		foundBetterPick = true;
	}

	for (int i = 0, sz = map->ents.size(); i < sz; i++) {
		if (renderEnts[i].modelIdx >= 0) {

			bool isSpecial = false;
			for (int k = 0; k < renderModels[renderEnts[i].modelIdx].groupCount; k++) {
				if (renderModels[renderEnts[i].modelIdx].renderGroups[k].transparent) {
					isSpecial = true;
					break;
				}
			}

			if (isSpecial && !(g_render_flags & RENDER_SPECIAL_ENTS)) {
				continue;
			} else if (!isSpecial && !(g_render_flags & RENDER_ENTS)) {
				continue;
			}

			if (pickPoly(start, dir, renderEnts[i].offset, renderEnts[i].modelIdx, pickInfo)) {
				pickInfo.entIdx = i;
				pickInfo.modelIdx = renderEnts[i].modelIdx;
				foundBetterPick = true;
			}
		}
		else if (g_render_flags & RENDER_POINT_ENTS) {
			vec3 mins = renderEnts[i].offset + renderEnts[i].pointEntCube->mins;
			vec3 maxs = renderEnts[i].offset + renderEnts[i].pointEntCube->maxs;
			if (pickAABB(start, dir, mins, maxs, pickInfo)) {
				pickInfo.entIdx = i;
				pickInfo.modelIdx = -1;
				pickInfo.faceIdx = -1;
				foundBetterPick = true;
			};
		}
	}

	return foundBetterPick;
}

bool BspRenderer::pickPoly(vec3 start, vec3 dir, vec3 offset, int modelIdx, PickInfo& pickInfo) {
	BSPMODEL& model = map->models[modelIdx];

	bool foundBetterPick = false;
	bool skipSpecial = !(g_render_flags & RENDER_SPECIAL);

	for (int k = 0; k < model.nFaces; k++) {
		FaceMath& faceMath = faceMaths[model.iFirstFace + k];
		BSPFACE& face = map->faces[model.iFirstFace + k];
		BSPPLANE& plane = map->planes[face.iPlane];
		vec3 planeNormal = faceMath.normal;
		float fDist = faceMath.fdist;
		
		if (skipSpecial && modelIdx == 0) {
			BSPTEXTUREINFO& info = map->texinfos[face.iTextureInfo];
			if (info.nFlags & TEX_SPECIAL) {
				continue;
			}
		}
		
		if (offset.x != 0 || offset.y != 0 || offset.z != 0) {
			vec3 newPlaneOri = offset + (planeNormal * fDist);
			fDist = dotProduct(planeNormal, newPlaneOri) / dotProduct(planeNormal, planeNormal);
		}

		float dot = dotProduct(dir, planeNormal);

		// don't select backfaces or parallel faces
		if (dot >= 0) {
			continue;
		}

		float t = dotProduct((planeNormal * fDist) - start, planeNormal) / dot;

		if (t < 0) {
			continue; // intersection behind camera
		}

		vec3 intersection = start + dir * t; // point where ray intersects the plane

		// transform to plane's coordinate system
		vec2 localRayPoint = (faceMath.worldToLocal * vec4(intersection, 1)).xy();

		static vec2 localVerts[128];
		for (int e = 0; e < faceMath.vertCount; e++) {
			localVerts[e] = (faceMath.worldToLocal * vec4(faceMath.verts[e] + offset, 1)).xy();
		}

		// check if point is inside the polygon using the plane's 2D coordinate system
		// https://stackoverflow.com/a/34689268
		bool inside = true;
		for (int i = 0; i < faceMath.vertCount; i++)
		{
			vec2& v1 = localVerts[i];
			vec2& v2 = localVerts[(i + 1) % faceMath.vertCount];

			if (v1.x == localRayPoint.x && v1.y == localRayPoint.y) {
				break; // on edge = inside
			}
			
			float d = (localRayPoint.x - v1.x) * (v2.y - v1.y) - (localRayPoint.y - v1.y) * (v2.x - v1.x);

			if (d < 0) {
				// point is outside of this edge
				inside = false;
				break;
			}
		}
		if (!inside) {
			continue;
		}

		if (t < pickInfo.bestDist) {
			foundBetterPick = true;
			pickInfo.bestDist = t;
			pickInfo.faceIdx = model.iFirstFace + k;
			pickInfo.valid = true;
		}
	}

	return foundBetterPick;
}

bool BspRenderer::pickAABB(vec3 start, vec3 rayDir, vec3 mins, vec3 maxs, PickInfo& pickInfo) {
	bool foundBetterPick = false;

	/*
	Fast Ray-Box Intersection
	by Andrew Woo
	from "Graphics Gems", Academic Press, 1990
	https://web.archive.org/web/20090803054252/http://tog.acm.org/resources/GraphicsGems/gems/RayBox.c
	*/

	bool inside = true;
	char quadrant[3];
	register int i;
	int whichPlane;
	double maxT[3];
	double candidatePlane[3];

	float* origin = (float*)&start;
	float* dir = (float*)&rayDir;
	float* minB = (float*)&mins;
	float* maxB = (float*)&maxs;
	float coord[3];

	const char RIGHT = 0;
	const char LEFT = 1;
	const char MIDDLE = 2;

	/* Find candidate planes; this loop can be avoided if
	rays cast all from the eye(assume perpsective view) */
	for (i = 0; i < 3; i++) {
		if (origin[i] < minB[i]) {
			quadrant[i] = LEFT;
			candidatePlane[i] = minB[i];
			inside = false;
		}
		else if (origin[i] > maxB[i]) {
			quadrant[i] = RIGHT;
			candidatePlane[i] = maxB[i];
			inside = false;
		}
		else {
			quadrant[i] = MIDDLE;
		}
	}

	/* Ray origin inside bounding box */
	if (inside) {
		return false;
	}

	/* Calculate T distances to candidate planes */
	for (i = 0; i < 3; i++) {
		if (quadrant[i] != MIDDLE && dir[i] != 0.0f)
			maxT[i] = (candidatePlane[i] - origin[i]) / dir[i];
		else
			maxT[i] = -1.0f;
	}

	/* Get largest of the maxT's for final choice of intersection */
	whichPlane = 0;
	for (i = 1; i < 3; i++) {
		if (maxT[whichPlane] < maxT[i])
			whichPlane = i;
	}

	/* Check final candidate actually inside box */
	if (maxT[whichPlane] < 0.0f)
		return false;
	for (i = 0; i < 3; i++) {
		if (whichPlane != i) {
			coord[i] = origin[i] + maxT[whichPlane] * dir[i];
			if (coord[i] < minB[i] || coord[i] > maxB[i])
				return false;
		}
		else {
			coord[i] = candidatePlane[i];
		}
	}
	/* ray hits box */

	vec3 intersectPoint(coord[0], coord[1], coord[2]);
	float dist = (intersectPoint - start).length();

	if (dist < pickInfo.bestDist) {
		pickInfo.bestDist = dist;
		pickInfo.valid = true;
		return true;
	}

	return false;
}