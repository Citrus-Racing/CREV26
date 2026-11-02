import paho.mqtt.client as mqtt
import struct
import time
from typing import Callable, Optional

class MQTTHandler:
    """Handles MQTT communication for Teensy motor control."""
    
    def __init__(self, broker_ip: str, broker_port: int = 1883):
        self.broker_ip = broker_ip
        self.broker_port = broker_port
        self.client = mqtt.Client()
        self.connected = False
        self.message_callback: Optional[Callable] = None
        
        # Set up callbacks
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
    
    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            print(f"✓ Connected to MQTT broker at {self.broker_ip}:{self.broker_port}")
        else:
            print(f"✗ Connection failed with code {rc}")
    
    def _on_message(self, client, userdata, msg):
        if self.message_callback:
            self.message_callback(msg)
    
    def _on_disconnect(self, client, userdata, rc):
        if rc != 0:
            print(f"⚠ Unexpected disconnection (code {rc})")
        self.connected = False
    
    def connect(self):
        """Connect to the MQTT broker."""
        print(f"Connecting to {self.broker_ip}:{self.broker_port}...")
        self.client.connect(self.broker_ip, self.broker_port, keepalive=60)
        self.client.loop_start()
        
        # Wait for connection
        timeout = time.time() + 5
        while not self.connected and time.time() < timeout:
            time.sleep(0.1)
        
        if not self.connected:
            raise ConnectionError("Failed to connect to MQTT broker")
    
    def subscribe(self, topic: str, callback: Callable):
        """Subscribe to a topic and set the message callback."""
        self.message_callback = callback
        self.client.subscribe(topic)
        print(f"✓ Subscribed to: {topic}")
    
    def publish_effort(self, topic: str, effort: float):
        """Publish a motor effort command (little-endian float32, what the Teensy expects)."""
        payload = struct.pack('<f', effort)
        self.client.publish(topic, payload)
    
    def disconnect(self):
        """Disconnect from the broker."""
        self.client.loop_stop()
        self.client.disconnect()
        print("Disconnected from MQTT broker")

