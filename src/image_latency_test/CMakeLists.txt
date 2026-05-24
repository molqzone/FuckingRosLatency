cmake_minimum_required(VERSION 3.8)
project(image_latency_test)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-g -O3)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(builtin_interfaces REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/BenchImage1440.msg"
  "msg/BenchImage320.msg"
  DEPENDENCIES builtin_interfaces
)

rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")

add_executable(image_publisher_node
  src/image_publisher_node.cpp
)
ament_target_dependencies(image_publisher_node
  rclcpp
  sensor_msgs
)
target_link_libraries(image_publisher_node "${cpp_typesupport_target}")

add_executable(image_subscriber_node
  src/image_subscriber_node.cpp
)
ament_target_dependencies(image_subscriber_node
  rclcpp
  sensor_msgs
)
target_link_libraries(image_subscriber_node "${cpp_typesupport_target}")

add_executable(intra_process_image_latency
  src/intra_process_image_latency.cpp
)
ament_target_dependencies(intra_process_image_latency
  rclcpp
  sensor_msgs
)
target_link_libraries(intra_process_image_latency "${cpp_typesupport_target}")

install(TARGETS
  image_publisher_node
  image_subscriber_node
  intra_process_image_latency
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include/
)

ament_export_dependencies(rosidl_default_runtime)

ament_package()
