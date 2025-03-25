
# Train and deploy a meter reading model on ESP32-S3
## Introduction

This tutorial shows how to train and deploy a meter reading detection model on ESP32-S3.

## Prerequisites

### Hardware
- PC with Linux or Mac OS, WSL2 is also supported.
- ESP32-S3 development board
- USB cable

### Software
- [EdgeLab](https://edgelab.readthedocs.io/zh_CN/latest/)
- [ESP-IDF](./get_start.md#how-to-install)
- [edgelab-example-esp32](https://github.com/Seeed-Studio/edgelab-example-esp32)

```{note}
Plesae make sure that you have installed the latest version of EdgeLab & ESP-IDF.
```

## Train the model

### Prepare the dataset

Use the provided dataset to train the model.

```bash
cd EdgeLab
mkdir -p datasets
cd datasets
wget https://files.seeedstudio.com/wiki/Edgelab/meter.zip
unzip meter.zip
```

### Prepare the configuration file

```python
_base_ = '../_base_/default_runtime.py'

num_classes=1
model = dict(type='PFLD',
             backbone=dict(type='PfldMobileNetV2',
                           inchannel=3,
                           layer1=[16, 16, 16, 16, 16],
                           layer2=[32, 32, 32, 32, 32, 32],
                           out_channel=16),
             head=dict(
                 type='PFLDhead',
                 num_point=num_classes,
                 input_channel=16,
             ),
             loss_cfg=dict(type='PFLDLoss'))


# dataset settings
dataset_type = 'MeterData'

data_root = './work_dirs/datasets/meter'
height=112
width=112
batch_size=32
workers=4

train_pipeline = [
    dict(type="Resize", height=height, width=width, interpolation=0),
    dict(type='ColorJitter', brightness=0.3, p=0.5),
    # dict(type='GaussNoise'),
    dict(type='MedianBlur', blur_limit=3, p=0.3),
    dict(type='HorizontalFlip'),
    dict(type='VerticalFlip'),
    dict(type='Rotate'),
    dict(type='Affine', translate_percent=[0.05, 0.1], p=0.6)
]

val_pipeline = [dict(type="Resize", height=height, width=width)]



data = dict(
    samples_per_gpu=batch_size,
    workers_per_gpu=workers,
    train=dict(type=dataset_type,
               data_root=data_root,
               index_file=r'train/annotations.txt',
               pipeline=train_pipeline,
               test_mode=False),
    val=dict(type=dataset_type,
             data_root=data_root,
             index_file=r'val/annotations.txt',
             pipeline=val_pipeline,
             test_mode=True),
    test=dict(type=dataset_type,
              data_root=data_root,
              index_file=r'val/annotations.txt',
              pipeline=val_pipeline,
              test_mode=True
              # dataset_info={{_base_.dataset_info}}
              ))


lr=0.0001
epochs=300
evaluation = dict(save_best='loss')
optimizer = dict(type='Adam', lr=lr, betas=(0.9, 0.99), weight_decay=1e-6)
optimizer_config = dict(grad_clip=dict(max_norm=35, norm_type=2))
# learning policy
lr_config = dict(policy='step',
                 warmup='linear',
                 warmup_iters=400,
                 warmup_ratio=0.0001,
                 step=[400, 440, 490])
total_epochs = epochs
find_unused_parameters = True
```

Save as EdgeLab/configs/pfld/pfld_mv2n_112_custom.py

### Train the model

#### Download the pre-trained model.
```bash
cd EdgeLab
mkdir -p work_dirs/pretrain/ && cd work_dirs/pretrain
wget  https://github.com/Seeed-Studio/EdgeLab/releases/download/model_zoo/pfld_mv2n_112.pth 
```

#### Start training.
```bash
cd EdgeLab
conda activate edgelab
python tools/train.py mmpose configs/pfld/pfld_mv2n_112_custom.py --gpus=1 --cfg-options total_epochs=50 load_from=./work_dirs/pretrain/pfld_mv2n_112.pth 
```

## Convert the model
Convert models to TensorFlow Lite.

```bash
cd EdgeLab
conda activate edgelab
python tools/torch2tflite.py mmpose  configs/pfld/pfld_mv2n_112_custom.py --weights work_dirs/pfld_mv2n_112_custom/exp1/latest.pth --tflite_type int8 
```

```{note}
Note: The exp1 in the path is generated in the first training. If you train multiple times, expx will be added one by one.
```

## Deploy the model

Convert the model to a C file and put it in the `components/modules/model` directory of `edgelab-example-esp32`.

```bash
cd edgelab-example-esp32
conda activate edgelab
python tools/tflite2c.py --input work_dirs/pfld_mv2n_112/exp1/latest.tflite --name pfld_meter --output_dir ./components/modules/model
```

```{note}
Note: The exp1 in the path is generated in the first training. If you train multiple times, expx will be added one by one.
```

Compile and flash the program to the ESP32-S3 development board.

```bash
cd edgelab-example-esp32/examples/meter
idf.py set-target esp32s3
idf.py build
```

### Run 

![meter](../_static/esp32/images/meter_reading.gif)