
// Sparkfveun 9DOF Razor IMU AHRS
// 9 Degree of Measurement Attitude and Heading Reference System
// Firmware v1.0
//
// Released under Creative Commons License 
// Code by Doug Weibel and Jose Julio
// Based on ArduIMU v1.5 by Jordi Munoz and William Premerlani, Jose Julio and Doug Weibel

// Axis definition: 
   // X axis pointing forward (to the FTDI connector)
   // Y axis pointing to the right 
   // and Z axis pointing down.
// Positive pitch : nose up
// Positive roll : right wing down
// Positive yaw : clockwise

/* Hardware version - v13
	
	ATMega328@3.3V w/ external 8MHz resonator
	High Fuse DA
        Low Fuse FF
	
	ADXL345: Accelerometer
	HMC5843: Magnetometer
	LY530:	Yaw Gyro
	LPR530:	Pitch and Roll Gyro

        Programmer : 3.3v FTDI
        Arduino IDE : Select board  "Arduino Duemilanove w/ATmega328"
*/
// This code works also on ATMega168 Hardware

#include <stdlib.h>
#include <stdio.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <Wire.h>
#include <SPI.h>
#include "pins_arduino.h"

// ADXL345 Sensitivity(from datasheet) => 4mg/LSB   1G => 1000mg/4mg = 256 steps
// Tested value : 248
#define GRAVITY 248  //this equivalent to 1G in the raw data coming from the accelerometer 
#define Accel_Scale(x) x*(GRAVITY/9.81)//Scaling the raw data of the accel to actual acceleration in meters for seconds square

#define ToRad(x) (x*0.01745329252)  // *pi/180
#define ToDeg(x) (x*57.2957795131)  // *180/pi

// LPR530 & LY530 Sensitivity (from datasheet) => (3.3mv at 3v)at 3.3v: 3mV/º/s, 3.22mV/ADC step => 0.93
// Tested values : 0.92
#define Gyro_Gain_X 0.92 //X axis Gyro gain
#define Gyro_Gain_Y 0.92 //Y axis Gyro gain
#define Gyro_Gain_Z 0.92 //Z axis Gyro gain
#define Gyro_Scaled_X(x) x*ToRad(Gyro_Gain_X) //Return the scaled ADC raw data of the gyro in radians for second
#define Gyro_Scaled_Y(x) x*ToRad(Gyro_Gain_Y) //Return the scaled ADC raw data of the gyro in radians for second
#define Gyro_Scaled_Z(x) x*ToRad(Gyro_Gain_Z) //Return the scaled ADC raw data of the gyro in radians for second

#define Kp_ROLLPITCH 0.02
#define Ki_ROLLPITCH 0.00002
#define Kp_YAW 1.2
#define Ki_YAW 0.00002

/*For debugging purposes*/
//OUTPUTMODE=1 will print the corrected data, 
//OUTPUTMODE=0 will print uncorrected data of the gyros (with drift)
#define OUTPUTMODE 1

//#define PRINT_DCM 0     //Will print the whole direction cosine matrix
#define PRINT_ANALOGS 0 //Will print the analog raw data
#define PRINT_ACCEL 1
#define PRINT_EULER 1   //Will print the Euler angles Roll, Pitch and Yaw
#define ENABLE_SPI 0  // Enable SPI Master - Disable Serial

#define ADC_WARM_CYCLES 50
#define STATUS_LED 13 

int8_t sensors[3] = {1,2,0};  // Map the ADC channels gyro_x, gyro_y, gyro_z
int SENSOR_SIGN[9] = {-1,1,-1,1,1,1,-1,-1,-1};  //Correct directions x,y,z - gyros, accels, magnetormeter

float G_Dt=0.02;    // Integration time (DCM algorithm)  We will run the integration loop at 50Hz if possible

long timer=0;   //general purpuse timer
long timer_old;
long timer24=0; //Second timer used to print values 
int AN[6]; //array that store the 3 ADC filtered data (gyros)
int AN_OFFSET[6]={0,0,0,0,0,0}; //Array that stores the Offset of the sensors
int ACC[3];          //array that store the accelerometers data

int accel_x;
int accel_y;
int accel_z;
int magnetom_x;
int magnetom_y;
int magnetom_z;
float MAG_Heading;

float Accel_Vector[3]= {0,0,0}; //Store the acceleration in a vector
float Gyro_Vector[3]= {0,0,0};//Store the gyros turn rate in a vector
float Omega_Vector[3]= {0,0,0}; //Corrected Gyro_Vector data
float Omega_P[3]= {0,0,0};//Omega Proportional correction
float Omega_I[3]= {0,0,0};//Omega Integrator
float Omega[3]= {0,0,0};

// Euler angles
float roll;
float pitch;
float yaw;

float errorRollPitch[3]= {0,0,0}; 
float errorYaw[3]= {0,0,0};

unsigned int counter=0;
byte gyro_sat=0;

float DCM_Matrix[3][3]= {
  {
    1,0,0  }
  ,{
    0,1,0  }
  ,{
    0,0,1  }
}; 
float Update_Matrix[3][3]={{0,1,2},{3,4,5},{6,7,8}}; //Gyros here


float Temporary_Matrix[3][3]={
  {
    0,0,0  }
  ,{
    0,0,0  }
  ,{
    0,0,0  }
};
 
//ADC variables
volatile uint8_t MuxSel=0;
volatile uint8_t analog_reference;
volatile uint16_t analog_buffer[8];
volatile uint8_t analog_count[8];

static int uart_putchar(char c, FILE *stream)
{
    if (c == '\n') uart_putchar('\r', stream);
  
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = c;
    
    return 0;
}

static FILE mystdout = {0};

unsigned int UART_Init(unsigned int ubrr)
{
        fdev_setup_stream(&mystdout, uart_putchar, NULL, _FDEV_SETUP_WRITE);
	int ubrr_new;
	// set baud rate
	ubrr_new = ubrr; 
	UBRR0H = ubrr_new>>8;
	UBRR0L = ubrr_new;
	
	// Enable receiver and transmitter 
	UCSR0A = (1<<U2X0); //double the speed
	UCSR0B = (1<<RXEN0)|(1<<TXEN0);
	
	// Set frame format: 8 bit, no parity, 1 stop bit,   
	UCSR0C = (1<<UCSZ00)|(1<<UCSZ01);
	
	stdout = &mystdout; //Required for printf init
	return(ubrr);
}

void setup()
{
#if ENABLE_SPI == 0
  DDRD = 0b00000010; //PORTD (TX output on PD1)
  UART_Init(16);
#endif
  pinMode (STATUS_LED,OUTPUT);  // Status LED

#if ENABLE_SPI == 1
  digitalWrite(SS, HIGH);  // ensure SS stays high for now
  // start the SPI library:
  SPI.begin();
  // Slow down the master a bit
  SPI.setClockDivider(SPI_CLOCK_DIV8);
#endif
 
  Analog_Reference(DEFAULT); 
  Analog_Init();
  I2C_Init();
  Accel_Init();
  Read_Accel();

#if ENABLE_SPI == 1
  spi_transfer_str("Sparkfun 9DOF Razor AHRS");
  spi_println();
#else
  printf("Sparkfun 9DOF Razor AHRS\n");
#endif

  digitalWrite(STATUS_LED,LOW);
  delay(1500);
 
  // Magnetometer initialization
  Compass_Init();
  
  // Initialze ADC readings and buffers
  Read_adc_raw();
  delay(20);
  
  for(int i=0;i<32;i++)    // We take some readings...
    {
    Read_adc_raw();
    Read_Accel();
    for(int y=0; y<6; y++)   // Cumulate values
      AN_OFFSET[y] += AN[y];
    delay(20);
    }
    
  for(int y=0; y<6; y++)
    AN_OFFSET[y] = AN_OFFSET[y]/32;
    
  AN_OFFSET[5]-=GRAVITY*SENSOR_SIGN[5];

  //Serial.println("Offset:");
  for(int y=0; y<6; y++) {
#if ENABLE_SPI == 1
  spi_transfer_int(AN_OFFSET[y]);
  spi_println();
#else
  printf("%d\n", AN_OFFSET[y]);
#endif
  }
  delay(2000);
  digitalWrite(STATUS_LED,HIGH);
    
  Read_adc_raw();     // ADC initialization
  timer=millis();
  delay(20);
  counter=0;
}

void loop() //Main Loop
{

#if ENABLE_SPI == 1
  // enable Slave Select
  digitalWrite(SS, LOW);    // SS is pin 10
#endif 
  
  if((millis()-timer)>=20)  // Main loop runs at 50Hz
  {
    counter++;
    timer_old = timer;
    timer=millis();
    if (timer>timer_old)
      G_Dt = (timer-timer_old)/1000.0;    // Real time of loop run. We use this on the DCM algorithm (gyro integration time)
    else
      G_Dt = 0;
    
    // *** DCM algorithm
    // Data adquisition
    Read_adc_raw();   // This read gyro data
    Read_Accel();     // Read I2C accelerometer
    
    if (counter > 5)  // Read compass data at 10Hz... (5 loop runs)
      {
      counter=0;
      Read_Compass();    // Read I2C magnetometer
      Compass_Heading(); // Calculate magnetic heading  
      }
    
    // Calculations...
    Matrix_update(); 
    Normalize();
    Drift_correction();
    Euler_angles();
    // ***

#if ENABLE_SPI == 1
    spi_printdata();
#else
    printdata();
#endif
    
    //Turn off the LED when you saturate any of the gyros.
    if((abs(Gyro_Vector[0])>=ToRad(300))||(abs(Gyro_Vector[1])>=ToRad(300))||(abs(Gyro_Vector[2])>=ToRad(300)))
      {
      if (gyro_sat<50)
        gyro_sat+=10;
      }
    else
      {
      if (gyro_sat>0)
        gyro_sat--;
      }
  
    if (gyro_sat>0)
      digitalWrite(STATUS_LED,LOW);  
    else
      digitalWrite(STATUS_LED,HIGH);  
  
  }

#if ENABLE_SPI == 1
  // disable Slave Select
  digitalWrite(SS, HIGH);
#endif 
   
}
