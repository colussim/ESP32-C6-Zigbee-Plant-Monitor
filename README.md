![ESP32](https://img.shields.io/badge/ESP32-Embedded-red) ![ESP-IDF](https://img.shields.io/badge/Framework-ESP--IDF-blue) ![Language](https://img.shields.io/badge/Language-C-informational)

## 🛠 Installation ESP-IDF (macOS)

```bash
xcode-select --install

mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git fetch --tags
git checkout v5.4.1
git submodule update --init --recursive
./install.sh esp32s3
. ./export.sh

idf.py --version
ESP-IDF v5.4.1
```

Project creation + adding the Component zigbee

```bash

idf.py create-project sensor-plant-zigbee 
cd sensor-plant-zigbee 
idf.py set-target esp32c6

idf.py add-dependency "espressif/esp-zigbee-lib"
idf.py add-dependency "espressif/esp-zboss-lib"

idf.py reconfigure

```

Add support zigbee

idf.py menuconfig

go to Component config 
enable Zigbee 
Dans menuconfig :

Component config → Zigbee → Configure the Zigbee device type
	•	✅ Choisis Zigbee End Device (ZED) (ou “End device”)

---

comment rendre ton capteur “reconnu” et bien affiché dans Zigbee2MQTT :

il existe deux methode une méthode locale ou officielle en soumettant une Pull Request (PR) sur le github de Zigbee2MQTT pour que votre device soit integré.

Nous allons utiliser la methode Local : 
au niveau ou se trouve votre fichier de configuration de Zigbee2MQTT nous allons creer deux repertoires : 
- device_icons (ou sera stockee l'image au format png de notre device)
- external_converters  (ou sera stocker notre de fichier de definition du devices)

Avant de commencer nous devons récupèrer les infos d’identification du device
Dans Zigbee2MQTT, va dans l’onglet “Appareils” et clique sur ton capteur. Note :

le modèle Zigbee (ex : modelID, ici SoilSensor ou similaire)
le fabricant (manufacturer)

Crée un fichier de définition custom (sleepyPlantSensor.js )
Dans le dossier  :external_converters

```js
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;

module.exports = {
    zigbeeModel: ['SoilSensor'], //ZigbeeModel
    model: 'SoilSensor',
    vendor: 'ECHOME',
    description: 'Capteur humidité sol Zigbee DIY',
    fromZigbee: [fz.humidity, fz.battery],
    toZigbee: [],
    exposes: [
        e.humidity(),
        e.battery(),
        e.voltage(),
    ],
};
```

Ajoute la définition à Zigbee2MQTT

Dans configuration.yaml de Zigbee2MQTT, ajoute :

```yaml
external_converters:
  - external_converters/sleepyPlantSensor.js
```

Pour avoir l'icone de notre device , positionne l'icone souhaiter au format png dans le repertoire :external_converters  

Dans votre configuration.yaml, sous l'entrée de votre appareil, ajoutez le chemin relatif :

```yaml
'0x9c9e6efffe771b64':
    friendly_name: Sonde_int_Dracaena
    icon: device_icons/soilbeetle.png
```

## 📚 References

[ESP-Zigbee-SDK ](https://github.com/espressif/esp-zigbee-sdk)