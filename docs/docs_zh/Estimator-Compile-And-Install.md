# Estimator源代码编译&安装

## 开发环境准备

**CPU Base Docker Image**

| GCC Version | Python Version |                           IMAGE                           |
| ----------- | -------------- | --------------------------------------------------------- |
|   7.5.0     |    3.6.9       | alideeprec/deeprec-base:deeprec-base-cpu-py36-ubuntu18.04 |
|   9.4.0     |    3.8.10      | alideeprec/deeprec-base:deeprec-base-cpu-py38-ubuntu20.04 |
|   11.2.0    |    3.8.6       | alideeprec/deeprec-base:deeprec-base-cpu-py38-ubuntu22.04 |


**GPU Base Docker Image**

| GCC Version | Python Version | CUDA VERSION |                           IMAGE                                 |
| ----------- | -------------- | ------------ | --------------------------------------------------------------- |
|    7.5.0    |    3.6.9       | CUDA 11.6.1  | alideeprec/deeprec-base:deeprec-base-gpu-py36-cu116-ubuntu18.04 |
|    9.4.0    |    3.8.10      | CUDA 11.6.2  | alideeprec/deeprec-base:deeprec-base-gpu-py38-cu116-ubuntu20.04 |
|    11.2.0   |    3.8.6       | CUDA 11.7.1  | alideeprec/deeprec-base:deeprec-base-gpu-py38-cu117-ubuntu22.04 |


**CPU Dev Docker (with bazel cache)**

| GCC Version | Python Version |                           IMAGE                           |
| ----------- | -------------- | --------------------------------------------------------- |
|   7.5.0     |    3.6.9       | alideeprec/deeprec-build:deeprec-dev-cpu-py36-ubuntu18.04 |
|   9.4.0     |    3.8.10      | alideeprec/deeprec-build:deeprec-dev-cpu-py38-ubuntu20.04 |

**GPU(cuda11.6) Dev Docker (with bazel cache)**

| GCC Version | Python Version | CUDA VERSION |                           IMAGE                                 |
| ----------- | -------------- | ------------ | --------------------------------------------------------------- |
|    7.5.0    |    3.6.9       | CUDA 11.6.1  | alideeprec/deeprec-build:deeprec-dev-gpu-py36-cu116-ubuntu18.04 |
|    9.4.0    |    3.8.10      | CUDA 11.6.2  | alideeprec/deeprec-build:deeprec-dev-gpu-py38-cu116-ubuntu20.04 |

## Estimator代码库及分支

由于DeepRec新增了分布式grpc++、star_server等protocol，在使用DeepRec配合原生Estimator会存在像grpc++, star_server功能使用时无法通过Estimator检查的问题，因为我们提供了针对DeepRec版本的Estimator.

代码库：[https://github.com/DeepRec-AI/estimator](https://github.com/DeepRec-AI/estimator)

开发分支：master，最新Release分支：deeprec2306

## Estimator编译

**代码编译**

```bash
bazel build //tensorflow_estimator/tools/pip_package:build_pip_package
```

**生成Wheel包**

```bash
bazel-bin/tensorflow_estimator/tools/pip_package/build_pip_package /tmp/estimator_whl
```

## Estimator安装

安装DeepRec会默认安装原生的tensorflow-estimator的版本，请重装新编译的tensorflow-estimator即可。

