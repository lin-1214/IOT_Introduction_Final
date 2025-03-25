# Copyright (c) Seeed Technology Co.,Ltd. All rights reserved.
_base_ = ['./base_arch.py']

# ========================Suggested optional parameters========================
# MODEL
num_classes = 2
deepen_factor = 1.0
widen_factor = 1.0

# DATA
dataset_type = 'sscma.CustomYOLOv5CocoDataset'
train_ann = 'training/_annotations.coco.json'
train_data = 'training/'  # Prefix of train image path
val_ann = 'testing/_annotations.coco.json'
val_data = 'testing/'  # Prefix of val image path

# dataset link: https://universe.roboflow.com/team-roboflow/coco-128
data_root = 'datasets/new_data'
height = 320
width = 320
batch = 16
workers = 5
val_batch = batch
val_workers = workers
imgsz = (width, height)

# TRAIN
persistent_workers = True

lr = 1e-6
weight_decay = 0.001
# SGD momentum/Adam beta1
# momentum = 0.937
optim_wrapper = dict(
    type='OptimWrapper',
    optimizer=dict(type='Adam', lr=lr, weight_decay=weight_decay),
    constructor='YOLOv5OptimizerConstructor',
)
# ================================END=================================
val_interval = 500
save_interval = 500

# DATA
affine_scale = 0.5
# MODEL
strides = [8, 16, 21]

anchors = [
    [(10, 13), (16, 30), (33, 23)],  # P3/8
    [(30, 61), (62, 45), (59, 119)],  # P4/16
    [(116, 90), (156, 198), (373, 326)],  # P5/32
]

model = dict(
    type='sscma.YOLODetector',
    backbone=dict(
        type='FastShuffleNetV2',
        stage_repeats=[4, 8, 4],
        stage_out_channels=[-1, 24, 48, 96, 192],
        init_cfg=dict(
            type='Pretrained',
            checkpoint='https://files.seeedstudio.com/sscma/model_zoo/backbone/fastshufllenet2_sha1_90be6b843860adcc72555d8699dafaf99624bddd.pth',
        ),
        _delete_=True,
    ),
    neck=dict(
        type='YOLOv5PAFPN',
        deepen_factor=deepen_factor,
        widen_factor=widen_factor,
        in_channels=[48, 96, 192],
        out_channels=[48, 96, 192],
        num_csp_blocks=1,
        # norm_cfg=norm_cfg,
        act_cfg=dict(type='ReLU', inplace=True),
    ),
    bbox_head=dict(
        head_module=dict(
            num_classes=num_classes,
            in_channels=[48, 96, 192],
            widen_factor=widen_factor,
        ),
    ),
)

# ======================datasets==================


batch_shapes_cfg = dict(
    type='BatchShapePolicy',
    batch_size=1,
    img_size=imgsz[0],
    # The image scale of padding should be divided by pad_size_divisor
    size_divisor=32,
    # Additional paddings for pixel scale
    extra_pad_ratio=0.5,
)

albu_train_transforms = [
    dict(type='Blur', p=0.01),
    dict(type='MedianBlur', p=0.01),
    dict(type='ToGray', p=0.01),
    dict(type='CLAHE', p=0.01),
]

pre_transform = [
    dict(type='LoadImageFromFile', file_client_args=dict(backend='disk')),
    dict(type='LoadAnnotations', with_bbox=True),
]

train_pipeline = [
    *pre_transform,
    dict(type='Mosaic', img_scale=imgsz, pad_val=114.0, pre_transform=pre_transform),
    dict(
        type='YOLOv5RandomAffine',
        max_rotate_degree=0.0,
        max_shear_degree=0.0,
        scaling_ratio_range=(1 - affine_scale, 1 + affine_scale),
        # imgsz is (width, height)
        border=(-imgsz[0] // 2, -imgsz[1] // 2),
        border_val=(114, 114, 114),
    ),
    dict(
        type='mmdet.Albu',
        transforms=albu_train_transforms,
        bbox_params=dict(type='BboxParams', format='pascal_voc', label_fields=['gt_bboxes_labels', 'gt_ignore_flags']),
        keymap={'img': 'image', 'gt_bboxes': 'bboxes'},
    ),
    dict(type='YOLOv5HSVRandomAug'),
    dict(type='mmdet.RandomFlip', prob=0.5),
    dict(
        type='mmdet.PackDetInputs', meta_keys=('img_id', 'img_path', 'ori_shape', 'img_shape', 'flip', 'flip_direction')
    ),
]

train_dataloader = dict(
    batch_size=batch,
    num_workers=workers,
    persistent_workers=persistent_workers,
    pin_memory=True,
    sampler=dict(type='DefaultSampler', shuffle=True),
    dataset=dict(
        type=dataset_type,
        data_root=data_root,
        ann_file=train_ann,
        data_prefix=dict(img=train_data),
        filter_cfg=dict(filter_empty_gt=False, min_size=32),
        pipeline=train_pipeline,
    ),
)

test_pipeline = [
    dict(type='LoadImageFromFile', file_client_args=dict(backend='disk')),
    dict(type='YOLOv5KeepRatioResize', scale=imgsz),
    dict(type='LetterResize', scale=imgsz, allow_scale_up=False, pad_val=dict(img=114)),
    dict(type='LoadAnnotations', with_bbox=True, _scope_='mmdet'),
    dict(
        type='mmdet.PackDetInputs',
        meta_keys=('img_id', 'img_path', 'ori_shape', 'img_shape', 'scale_factor', 'pad_param'),
    ),
]

val_dataloader = dict(
    batch_size=val_batch,
    num_workers=val_workers,
    persistent_workers=persistent_workers,
    pin_memory=True,
    drop_last=False,
    sampler=dict(type='DefaultSampler', shuffle=False),
    dataset=dict(
        type=dataset_type,
        data_root=data_root,
        test_mode=True,
        data_prefix=dict(img=val_data),
        ann_file=val_ann,
        pipeline=test_pipeline,
        batch_shapes_cfg=batch_shapes_cfg,
    ),
)

test_dataloader = val_dataloader
