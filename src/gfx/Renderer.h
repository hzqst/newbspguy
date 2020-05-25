#include "Bsp.h"
#include <GL/glew.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include <GLFW/glfw3.h>
#include "ShaderProgram.h"
#include "BspRenderer.h"
#include "Fgd.h"

class Renderer {
public:
	vector<BspRenderer*> mapRenderers;

	Renderer();
	~Renderer();

	void addMap(Bsp* map);

	void renderLoop();

	void imgui_demo();

private:
	GLFWwindow* window;
	ShaderProgram* bspShader;
	ShaderProgram* colorShader;
	PointEntRenderer* pointEntRenderer;

	Fgd* fgd;

	vec3 cameraOrigin;
	vec3 cameraAngles;
	bool cameraIsRotating;
	float frameTimeScale = 0.0f;
	float moveSpeed = 4.0f;
	float fov, zNear, zFar;
	mat4x4 model, view, projection, modelView, modelViewProjection;

	vec2 lastMousePos;
	vec3 pickStart, pickDir, pickEnd;

	int windowWidth;
	int windowHeight;

	bool vsync;
	bool showDebugWidget;
	bool showKeyvalueWidget;
	bool smartEdit;
	ImFont* smallFont;
	ImFont* largeFont;

	int oldLeftMouse;

	PickInfo pickInfo;

	vec3 getMoveDir();
	void cameraControls();
	void setupView();
	void getPickRay(vec3& start, vec3& pickDir);

	void drawLine(vec3 start, vec3 end, COLOR3 color);

	void drawGui();
	void drawMenuBar();
	void drawFpsOverlay();
	void drawDebugWidget();
	void drawKeyvalueEditor();
	void drawSmartEditEditor(Entity* ent);
};