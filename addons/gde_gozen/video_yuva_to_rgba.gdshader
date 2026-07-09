shader_type canvas_item;

uniform sampler2D y_data;
uniform sampler2D u_data;
uniform sampler2D v_data;
uniform sampler2D a_data;

uniform vec2 resolution;
uniform vec4 color_profile;
uniform bool full_color;
uniform int interlaced; // 0 = no, 1 = top first, 2 = bottom first.
uniform float rotation;
// Set by MediaPlayback: true only under the Forward+ renderer with HDR 2D, where
// the 2D canvas is composited in linear space. Every other configuration treats
// canvas colors as sRGB, matching this shader's YUV->RGB output; in the HDR 2D
// case that output must be decoded to linear to look the same.
uniform bool output_linear = false;

varying vec2 tex_uv;
varying vec4 modulate;

const vec3 LIMITED_Y_OFFSET = vec3(16.0/255.0, 128.0/255.0, 128.0/255.0);
const vec3 LIMITED_SCALE = vec3(255.0/219.0, 255.0/224.0, 255.0/224.0);

void vertex() {
	float c = cos(rotation);
	float s = sin(rotation);
	mat2 rot_mat = mat2(vec2(c, s), vec2(-s, c));

	tex_uv = (rot_mat * (UV - 0.5)) + 0.5;
	modulate = COLOR;
}

void fragment() {
	if (any(lessThan(tex_uv, vec2(0.0, 0.0))) || any(greaterThan(tex_uv, vec2(1.0, 1.0)))) {
		COLOR = vec4(0.0);
	} else {
		float y_val = texture(y_data, tex_uv).r;

		if (interlaced > 0) {
			float offset_dir = (interlaced == 1) ? -1.0 : 1.0;
			vec2 offset_uv = clamp(tex_uv + vec2(0.0, offset_dir / resolution.y), 0.0, 1.0);
			y_val = mix(y_val, texture(y_data, offset_uv).r, 0.5);
		}

		vec3 yuv = vec3(y_val, texture(u_data, tex_uv).r, texture(v_data, tex_uv).r);

		if (full_color) {
			yuv.yz -= 0.5;
		} else {
			yuv = (yuv - LIMITED_Y_OFFSET) * LIMITED_SCALE;
		}

		vec3 rgb = vec3(
			yuv.x + (yuv.z * color_profile.x),
			yuv.x - dot(yuv.yz, color_profile.yz),
			yuv.x + (yuv.y * color_profile.w)
		);

		if (output_linear) { // sRGB -> linear (IEC 61966-2-1), color channels only.
			rgb = clamp(rgb, 0.0, 1.0);
			rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), rgb));
		}

		COLOR = vec4(rgb, texture(a_data, tex_uv).r) * modulate;
	}
}
