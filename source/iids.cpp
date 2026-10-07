//==============================================================================
// iids.cpp — 实例化 VST3 各接口的 iid 静态成员
//
// 为什么需要这个文件：SDK 头文件用
//     DECLARE_CLASS_IID (IComponent, ...)
// 声明 `static const FUID IComponent::iid;`
// 但 DECLARE_CLASS_IID 默认不生成定义（取决于 INIT_CLASS_IID 宏）。
// 必须有一个 .cpp 先定义 INIT_CLASS_IID 再包含相关头，才会生成符号。
//
// 注意：SDK 自带的 base/coreiids.cpp 只覆盖基础接口，
// VST3 业务接口（IComponent / IAudioProcessor / IPlugView 等）必须在这里补。
// 因此构建时【不编译 coreiids.cpp】——否则基础接口会重复定义。
//==============================================================================

// 关键：先定义 INIT_CLASS_IID，让后续 DECLARE_CLASS_IID 生成实际定义
#define INIT_CLASS_IID

// 包含顺序无关紧要——DECLARE_CLASS_IID 在本 TU 里统一实例化。
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/ivstcontextmenu.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
