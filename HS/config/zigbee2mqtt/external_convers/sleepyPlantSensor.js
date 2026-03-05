const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;

module.exports = {
    zigbeeModel: ['SoilSensor'], //ZigbeeModel
    model: 'SoilSensor',
    vendor: 'ECHOME',
    description: 'DIY Zigbee soil moisture sensor',
    fromZigbee: [fz.humidity, fz.battery],
    toZigbee: [],
    exposes: [
        e.humidity(),
        e.battery(),
        e.voltage(),
    ],
};
