#pragma once
#include <math.h>
/* Shared pixel-space UI layout and inverse pointer mapping. */
typedef struct LT2UILayout { float scale, x, y; } LT2UILayout;
static LT2UILayout lt2_ui_layout(float width,float height,float logicalWidth,float logicalHeight,
                                float anchorX,float anchorY) {
    LT2UILayout out={1,0,0};
    if(width<=0 || height<=0 || logicalWidth<=0 || logicalHeight<=0)return out;
    out.scale=fminf(width/logicalWidth,height/logicalHeight);
    out.x=(width-logicalWidth*out.scale)*anchorX;
    out.y=(height-logicalHeight*out.scale)*anchorY;
    return out;
}
/* Camera projection and CPU frustum must see this same matrix. Orthographic
 * cameras (HUD/map) are deliberately excluded. Row-major game matrix layout. */
static int lt2_camera_fit(float *projection,float aspect) {
    if(!(aspect>0) || !isfinite(aspect) || projection[15]!=0 || projection[11]==0 ||
       !isfinite(projection[5]) || projection[5]<=0)return 0;
    float horizontal=projection[5]/aspect;
    if(fabsf(projection[0]-horizontal)<1e-6f)return 0;
    projection[0]=horizontal;
    return 1;
}
