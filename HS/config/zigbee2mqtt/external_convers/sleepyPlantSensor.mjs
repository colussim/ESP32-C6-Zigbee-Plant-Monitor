import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

export default {
    fingerprint: [
        {modelID: 'SoilSensor', manufacturerName: 'ECHOME'},
    ],
    zigbeeModel: ['SoilSensor'],
    model: 'SoilSensor',
    vendor: 'ECHOME',
    description: 'DIY Zigbee soil moisture sensor',
    icon: 'device_icons/soilbeetle.png',
    extend: [
        m.battery(),
        m.temperature(),
        m.illuminance(),
        m.humidity(),
    ],
};
