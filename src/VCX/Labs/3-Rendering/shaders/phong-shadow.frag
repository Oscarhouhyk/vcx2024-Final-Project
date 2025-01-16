#version 410 core

layout(location = 0) in  vec3 v_Position;
layout(location = 1) in  vec3 v_Normal;
layout(location = 2) in  vec2 v_TexCoord;
layout(location = 3) in  vec4 v_LightSpacePosition;

layout(location = 0) out vec4 f_Color;

struct Light {
    vec3  Intensity;
    vec3  Direction;   // For spot and directional lights.
    vec3  Position;    // For point and spot lights.
    float CutOff;      // For spot lights.
    float OuterCutOff; // For spot lights.
};

layout(std140) uniform PassConstants {
    mat4  u_Projection;
    mat4  u_View;
    vec3  u_ViewPosition;
    vec3  u_AmbientIntensity;
    Light u_Lights[4];
    int   u_CntPointLights;
    int   u_CntSpotLights;
    int   u_CntDirectionalLights;
};

uniform float u_AmbientScale;
uniform bool u_UseBlinn;
uniform sampler2D u_DiffuseMap;
uniform sampler2D u_SpecularMap;
uniform sampler2D u_ShadowMap;

uniform bool u_useUniform; // 用uniform sampling 还是 poisson sampling
uniform bool u_useSoftShadow; // hard or soft shadow
uniform bool u_usePCF; // 展示PCF结果还是PCSS结果

uniform float pcfBiasC; // 采样偏移量 // default 0.02
uniform float FILTER_RADIUS; // 采样半径 // default 10.0
uniform int poisson_ring_size = 1; //poisson sampling 圈数 // default 1

//ADJUST
#define SAMPLE_SIZE 50 // 采样点数量 // default 50
#define FRUSTUM_SIZE 800. // 视锥体大小 正交矩阵宽高 // default 800.
#define LIGHT_WORLD_SIZE 10. // 根据效果调节的光源在世界空间的大小 // default 10. 

#define SHADOW_MAP_SIZE 4096. // Framesize, cube map size 2048
#define NEAR_PLANE 0.01 // 光源所用透视矩阵的近平面数据 // ZNear 0.01 in Engine::Camera.hpp

#define EPS 1e-3
#define PI 3.141592653589793
#define PI2 6.283185307179586

highp float rand_1to1(highp float x ) { // from -1 to 1
  return fract(sin(x)*10000.0);
}

highp float rand_2to1(vec2 uv ) {   // from 0 to 1
  const highp float a = 12.9898, b = 78.233, c = 43758.5453;
  highp float dt = dot( uv.xy, vec2( a,b ) ), sn = mod( dt, PI );
  return fract(sin(sn) * c);
}

float unpack(vec4 rgbaDepth) {
    const vec4 bitShift = vec4(1.0, 1.0/255.0, 1.0/(255.0*255.0), 1.0/(255.0*255.0*255.0));
    return dot(rgbaDepth, bitShift);
}

vec2 poissonDisk[SAMPLE_SIZE];
void poissonDiskSampling( const in vec2 randomSeed ) {
  
  float inv_SAMPLE_SIZE = 1.0 / float( SAMPLE_SIZE );

  float angle = rand_2to1( randomSeed ) * PI2;
  float radius = inv_SAMPLE_SIZE;

  float angle_per_step = PI2 * float( poisson_ring_size ) / float( SAMPLE_SIZE );
  float radius_per_step = inv_SAMPLE_SIZE;

  for( int i = 0; i < SAMPLE_SIZE; i ++ ) {
    poissonDisk[i] = vec2( cos( angle ), sin( angle ) ) * pow( radius, 0.75 );
    radius += radius_per_step;
    angle += angle_per_step;
  }
}

void uniformDiskSampling( const in vec2 randomSeed ) {

  float randNum = rand_2to1(randomSeed);
  float sampleX = rand_1to1( randNum ) ;
  float sampleY = rand_1to1( sampleX ) ;

  float angle = sampleX * PI2;
  float radius = sqrt(sampleY);

  for( int i = 0; i < SAMPLE_SIZE; i ++ ) {
    poissonDisk[i] = vec2( radius * cos(angle) , radius * sin(angle)  );

    sampleX = rand_1to1( sampleY ) ;
    sampleY = rand_1to1( sampleX ) ;

    angle = sampleX * PI2;
    radius = sqrt(sampleY);
  }
}

float getShadowBias(float c, float filterRadiusUV){
  vec3 normal = normalize(v_Normal);
  vec3 lightDir = normalize(u_Lights[0].Position - v_Position);
  // 自适应bias
  float fragSize = (1. + ceil(filterRadiusUV)) * (FRUSTUM_SIZE / SHADOW_MAP_SIZE / 2.);
  return max(fragSize, fragSize * (1.0 - dot(normal, lightDir))) * c;
}

float useShadowMap(sampler2D shadowMap, vec4 shadowCoord, float biasC, float filterRadiusUV){
  float depth = unpack(texture(shadowMap, shadowCoord.xy));
  float cur_depth = shadowCoord.z;
  float bias = getShadowBias(biasC, filterRadiusUV);
  if(cur_depth - bias >= depth + EPS) return 0.;
  else return 1.0;
  
}

float PCF(sampler2D shadowMap, vec4 coords, float biasC, float filterRadiusUV) {

  if (!u_useUniform) uniformDiskSampling(coords.xy);
  else poissonDiskSampling(coords.xy); //使用xy坐标作为随机种子生成

  float res = 0.0;
  for(int i = 0; i < SAMPLE_SIZE; i++){
    vec2 offset = poissonDisk[i] * filterRadiusUV;
    float shadowDepth = useShadowMap(shadowMap, coords + vec4(offset, 0., 0.), biasC, filterRadiusUV);
    if(coords.z > shadowDepth + EPS)  res += 1.0;
  }
  
  return 1.0 - res / float(SAMPLE_SIZE);
}


float findBlocker(sampler2D shadowMap, vec2 uv, float zReceiver) {
  // 查找遮挡物，并求得平均深度
  
  poissonDiskSampling(uv);

  int blocker_cnt = 0;
  float block_depth_sum = 0.;

  float lightSize_uv = LIGHT_WORLD_SIZE / FRUSTUM_SIZE ; //光源在ShadowMap上的UV单位大小

  //float searchRadius = lightSize_uv * (v_LightSpacePosition.z - NEAR_PLANE) / v_LightSpacePosition.z;
  
  float searchRadius = FILTER_RADIUS / SHADOW_MAP_SIZE ;

  for(int i = 0; i < SAMPLE_SIZE; i++){
    float shadow_depth = unpack(texture(shadowMap, uv + poissonDisk[i] * searchRadius));
    if(zReceiver >= shadow_depth + EPS){
      block_depth_sum += shadow_depth;
      blocker_cnt++;
    }
  }

  if (blocker_cnt == 0)  return zReceiver;
  else  return block_depth_sum / float(blocker_cnt);
}

float PCSS(sampler2D shadowMap, vec4 coords, float biasC){
  //计算filter核大小，然后PCF

  float lightSize_uv = LIGHT_WORLD_SIZE / FRUSTUM_SIZE ; //光源在ShadowMap上的UV单位大小

  float avgBlockerDepth = findBlocker(shadowMap, coords.xy, coords.z);

  if(avgBlockerDepth < -EPS)
    return 1.0;

  float penumbra = (coords.z - avgBlockerDepth) * lightSize_uv / avgBlockerDepth;

  return PCF(shadowMap, coords, biasC, penumbra);
}


float Shadow(vec4 lightSpacePosition, vec3 normal, vec3 lightDir) {
    vec3 pos = lightSpacePosition.xyz / lightSpacePosition.w;
    pos.xyz = pos.xyz * 0.5 + 0.5;
    
    float closestDepth = 0;
    closestDepth = texture(u_ShadowMap, pos.xy).r;
    float shadow = 0.0;

    if (!u_useSoftShadow){ // hard shadow
      float curDepth = pos.z;
      float bias = max(1e-3 * (1.0 - dot(normal, lightDir)), 1e-4);
      shadow = (curDepth - bias > closestDepth ? 1.0 : 0.0);
    }
    else{ // soft shadow

      float filterRadiusUV = FILTER_RADIUS / SHADOW_MAP_SIZE; // PCF的采样filter范围
      if (u_usePCF)
        shadow = PCF(u_ShadowMap, vec4(pos, 1.0), pcfBiasC, filterRadiusUV);
      else 
        shadow = PCSS(u_ShadowMap, vec4(pos, 1.0), pcfBiasC);
    }

    if (pos.z > 1.0 || pos.x < -100. || pos.x > 100. || pos.y < -100. || pos.y > 100.) shadow = 0.0;
    return shadow;
}

vec3 Shade(vec3 lightIntensity, vec3 lightDir, vec3 normal, vec3 viewDir, vec3 diffuseColor, vec3 specularColor, float shininess) {
    // your code here:
    vec3 res = vec3(0);
    if(!u_UseBlinn)
        res = diffuseColor*(lightIntensity * max(0,dot(normal, lightDir))) 
        + specularColor*(lightIntensity * 
        pow(max(0, dot(reflect(-lightDir, normal), viewDir)), shininess));
    else
        res = diffuseColor*(lightIntensity * max(0,dot(normal, lightDir))) 
        + specularColor*(lightIntensity * 
        pow(max(0, dot(normal,normalize(viewDir+lightDir))), shininess));

    return res;
}

void main() {
    float gamma          = 2.2;
    vec4  diffuseFactor  = texture(u_DiffuseMap , v_TexCoord).rgba;
    vec4  specularFactor = texture(u_SpecularMap, v_TexCoord).rgba;
    if (diffuseFactor.a < .2) discard;
    vec3  diffuseColor   = pow(diffuseFactor.rgb, vec3(gamma));
    vec3  specularColor  = specularFactor.rgb;
    float shininess      = specularFactor.a * 256;
    vec3  normal         = normalize(v_Normal);
    vec3  viewDir        = normalize(u_ViewPosition - v_Position);

    // Ambient component.
    vec3  total = u_AmbientIntensity * u_AmbientScale * diffuseColor;

    // Only one light
    float shadow = Shadow(v_LightSpacePosition, normal, u_Lights[0].Direction);

    if (!u_useSoftShadow)
      total += (1. - shadow) * Shade(u_Lights[0].Intensity, u_Lights[0].Direction, normal, viewDir, diffuseColor, specularColor, shininess);
    else 
      total += shadow * Shade(u_Lights[0].Intensity, u_Lights[0].Direction, normal, viewDir, diffuseColor, specularColor, shininess);
   
    // Gamma correction.
    f_Color = vec4(pow(total, vec3(1. / gamma)), 1.);
}


