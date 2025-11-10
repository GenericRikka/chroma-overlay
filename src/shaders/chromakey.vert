#version 330 core
// Position in NDC will be computed from pixel coords on CPU and passed here.
// Simpler approach: pass directly in NDC already.
layout(location = 0) in vec2 aPos;     // NDC [-1,1]
layout(location = 1) in vec2 aTex;     // [0,1]

out vec2 vTex;

void main() {
    vTex = aTex;
    gl_Position = vec4(aPos, 0.0, 1.0);
}

