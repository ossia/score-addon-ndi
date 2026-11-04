/*{
  "ISFVSN": "2",
  "DESCRIPTION": "NDI matrix test picture: R grows to the right, G downwards, B constant, a white square at the top left, and an alpha ramp in the bottom tenth.",
  "INPUTS": []
}*/
void main() {
  // isf_FragNormCoord has y = 0 at the BOTTOM of the picture.
  float x = isf_FragNormCoord.x;
  float yTop = 1.0 - isf_FragNormCoord.y;
  vec3 c = vec3(x, yTop, 0.25);
  float a = 1.0;
  if(x < 0.1 && yTop < 0.1)
    c = vec3(1.0);
  // Alpha ramps left to right in the bottom strip: a format that carries
  // alpha keeps the ramp, one that does not makes the strip opaque.
  if(yTop >= 0.9)
    a = x;
  gl_FragColor = vec4(c, a);
}
