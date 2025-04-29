const binding = process._linkedBinding('electron_renderer_web_utils');

const webUtils = {
  getPathForFile: binding.getPathForFile,
  importExternalSharedTexture: binding.importExternalSharedTexture
};

export default webUtils;
