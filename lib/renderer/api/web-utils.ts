const binding = process._linkedBinding('electron_renderer_web_utils');

const webUtils = {
  getPathForFile: binding.getPathForFile,
  importExternalSharedTextureToGpuDevice: binding.importExternalSharedTextureToGpuDevice
};

export default webUtils;
