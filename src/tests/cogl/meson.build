subdir('conform')
subdir('unit')

if have_vulkan
  vulkan_unit_tests = [
    'test-vk-sync-unit',
    'test-vk-uniform-unit',
    'test-vk-pass-unit',
    'test-vk-device-unit',
    'test-vk-dmabuf-unit',
    'test-vk-software',
  ]
  foreach test_name: vulkan_unit_tests
    test_executable = executable(test_name,
      [test_name + '.c', 'test-vk-profile-stub.c'],
      c_args: ['-DCOGL_COMPILATION', '-ffunction-sections', '-fdata-sections'],
      link_args: ['-Wl,--gc-sections'],
      include_directories: [include_directories('../../..'), cogl_includepath, mtk_includepath],
      dependencies: [gio_dep, graphene_dep, pixman_dep, libdrm_dep, vulkan_dep, m_dep],
    )
    test(test_name, test_executable, suite: ['cogl', 'cogl/vulkan/unit'])
  endforeach
endif
