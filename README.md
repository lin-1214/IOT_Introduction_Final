# IoT_Introduction_Final
Final project for 2024 Fall Introduction to IoT @NTUEE

## Installation

1. Get submodules

```bash
git submodule update --init --recursive
```

2. Install SSCMA. Follow the instructions in [SSCMA](https://sensecraftma.seeed.cc/introduction/installation) from step 1 to step 4 to create a `sscma` virtual environment .

```bash
conda activate sscma
```

3. Install the dependencies of the RPi.

```bash
## On RPi
pip install -r requirements.txt
```

## Usage

1. Train your swift-yolo model with the config under ``, and use the following command to train, export and test the tflite format model.

```bash
### Under the ModelAssistant directory
### Please put your config file under the `configs/swift_yolo` directory
### And modify the `data_root` in the config file to the path of the dataset.
### Or you can use the default config file `configs/swift_yolo_new_crane.py`
## 1. Train
#  You should modify the config file to set the `data_root` to the path of the dataset.
#  The config file should be in coco dataset format.

sscma.train configs/swift_yolo/swift_yolo_new_crane.py \
    --work_dir new_crane \
    --cfg-options  \
        epochs=10000  \
        batch=128 \
        load_from="<path_to_pretrained_model>"

## 2. Export
#  Export the trained model to tflite format.
#  Including int8 and float32 quantization.

python3 tools/export.py \
    configs/swift_yolo/swift_yolo_new_crane.py \
    new_crane/epoch_10000.pth \
    --target tflite \
    --cfg-options \
        data_root="<path_to_dataset>"

## 3. Inference
#  Verify the exported tflite model on the dataset.

python3 tools/inference.py \
    configs/swift_yolo/swift_yolo_new_crane.py \
    new_crane/epoch_10000.pth \
    --cfg-options \
        data_root="<path_to_dataset>"
```

2. Compile the tflite model to the embedding c++ format.

```bash
## Put the model under the `esp32/model_zoo` directory
python3 tools/tflite2c.py --input model_zoo/<your_model>.tflite --name <your_name> --output_dir components/modules/model --classes='("tower","crane")'
```

And then you need to modify `esp32/components/modules/algorithm/algo_yolo.cpp` to include the header file of the model you just compiled. (Remember to modify the `#include` path and corresponding lines)

3. Compile the project and flash it to the ESP32 board.

```bash
idf.py build
idf.py -p <your_port> flash
```

4. Run the server on the RPi.

```bash
python3 server.py
```
