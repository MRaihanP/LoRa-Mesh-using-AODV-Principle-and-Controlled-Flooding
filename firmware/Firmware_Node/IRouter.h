#pragma once

struct IRouter {
  virtual ~IRouter() {}

  // Requests (deferred)
  virtual void requestHome()            = 0;  // MainPage
  virtual void requestMenu()            = 0;  // MenuPage

  virtual void requestSimpleChat()      = 0;
  virtual void requestNetwork()         = 0;
  virtual void requestLogPath()         = 0;
  virtual void requestLogData()         = 0;

  virtual void requestSensorMPU9250()   = 0;
  virtual void requestSensorBMP280()    = 0;
  virtual void requestSensorSHT30()     = 0;
  virtual void requestGNSS_M10()        = 0;
  virtual void requestClockDS3231()     = 0;
  virtual void requestMicroSD()         = 0;
  virtual void requestBattery()         = 0;
  virtual void requestWifi()            = 0;

  // Commit pending transition
  virtual void pumpAfterOverlayClosed() = 0;

  // Queries
  virtual bool inMenu()           const = 0;
  virtual bool inSimpleChat()     const = 0;
  virtual bool inNetwork()        const = 0;
  virtual bool inLogPath()        const = 0;
  virtual bool inLogData()        const = 0;

  virtual bool inSensorMPU9250()  const = 0;
  virtual bool inSensorBMP280()   const = 0;
  virtual bool inSensorSHT30()    const = 0;
  virtual bool inGNSS_M10()       const = 0;
  virtual bool inClockDS3231()    const = 0;
  virtual bool inMicroSD()        const = 0;
  virtual bool inBattery()        const = 0;
  virtual bool inWifi()           const = 0;
};