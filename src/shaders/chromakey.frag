#version 330 core
in vec2 vTex;
out vec4 fragColor;

uniform sampler2D uTex;

/* Optional tuning:
   uGreenMin: how high green must be to key
   uRedMax / uBlueMax: how low red/blue must be to key
   uFeather: soft edge radius (0 = hard)
*/
uniform float uGreenMin   = 0.60;
uniform float uRedMax     = 0.35;
uniform float uBlueMax    = 0.35;
uniform float uFeather    = 0.00; // start with 0 (hard discard)

void main() {
    vec4 c = texture(uTex, vTex);

    // Decide how "green" this pixel is
    float isGreen =
        float(c.g >= uGreenMin && c.r <= uRedMax && c.b <= uBlueMax);

    if (uFeather <= 0.0001) {
        // Fast, hard key
        if (isGreen > 0.5) discard;
        fragColor = vec4(c.rgb, 1.0);
    } else {
        // Soft key: fade alpha near threshold
        float gScore = smoothstep(uGreenMin - uFeather, uGreenMin + uFeather, c.g);
        float rScore = 1.0 - smoothstep(uRedMax - uFeather, uRedMax + uFeather, c.r);
        float bScore = 1.0 - smoothstep(uBlueMax - uFeather, uBlueMax + uFeather, c.b);
        float key = clamp(gScore * rScore * bScore, 0.0, 1.0); // 0..1 keyed
        float alpha = 1.0 - key; // keyed → transparent
        if (alpha <= 0.001) discard;
        fragColor = vec4(c.rgb, alpha);
    }
}

