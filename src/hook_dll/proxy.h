// NCMExclusive - 核心：COM 代理链 + 强制 WASAPI 独占模式
//
// 链路：CoCreateInstance(CLSID_MMDeviceEnumerator) 被 IAT 钩子拦截
//   -> EnumeratorProxy（包装 GetDefaultAudioEndpoint/GetDevice/EnumAudioEndpoints 返回值）
//   -> DeviceProxy::Activate(IAudioClient) -> ClientProxy
//   -> ClientProxy::Initialize：把宿主的共享模式请求改成独占模式
//
// 独占初始化要点（参考 AMExclusive / 微软 WASAPIRenderer 示例）：
//   1. IsFormatSupported(EXCLUSIVE) 预检，不支持则不强制（自动回退共享）
//   2. 独占必须带 AUDCLNT_STREAMFLAGS_EVENTCALLBACK；periodicity 必须非零
//   3. AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED 时按 GetBufferSize 换算周期，
//      并重新 Activate 一个全新客户端再重试（同一客户端只能 Initialize 一次）
//   4. 任何失败都静默回退到原始共享模式调用，保证播放不出问题
#pragma once

namespace ncmex {

// 在 hook DLL 的 worker 线程中调用：安装 IAT 钩子
void InstallHooks();

}  // namespace ncmex
