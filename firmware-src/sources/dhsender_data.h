/**
 *	\file		dhsender_data.h
 *	\brief		Declaration of data structs for dhsender, i.e. command results.
 *	\author		Nikolay Khabarov
 *	\date		2015
 *	\copyright	DeviceHive MIT
 */

#ifndef _DHSENDER_DATA_H_
#define _DHSENDER_DATA_H_

#include <stdarg.h>
#include "user_config.h"

/** Data type that should be read from arguments and how it will be formatted in response or notification. */
typedef enum {
	RDT_CONST_STRING,	///< Constant pointer to char should be passed. Will be formatted as string.
	RDT_DATA_WITH_LEN,	///< Pointer to data and integer length of data should be passed. Will be formatted as json with current data encoded method.
	RDT_FLOAT,			///< Float should be passed. Will be formatted as json with this value.
	RDT_GPIO,			///< Three 32bit value should be passed(caused, state, tick). Will be formatted as json.
	RDT_SEARCH64,		///< Data with groups of 64bit addresses. Pin number, pointer to data and integer length of data should be passed.
	RDT_FORMAT_STRING	///< Formated string, like sprintf. Text should be valid json.
} REQUEST_DATA_TYPE;

/** Request type. */
typedef enum {
	RT_RESPONCE_OK,		///< Response with success status.
	RT_RESPONCE_ERROR,	///< Response with error status.
	RT_NOTIFICATION		///< Notification, see REQUEST_NOTIFICATION_TYPE for available notification types.
} REQUEST_TYPE;

/** Notification type. */
typedef enum {
	RNT_NOTIFICATION_NONE,		///< Dummy, just to pass it for responses.
	RNT_NOTIFICATION_GPIO,		///< Notification will be marked as GPIO.
	RNT_NOTIFICATION_ADC,		///< Notification will be marked as ADC.
	RNT_NOTIFICATION_UART,		///< Notification will be marked as UART.
	RNT_NOTIFICATION_ONEWIRE	///< Notification will be marked as onewire.
} REQUEST_NOTIFICATION_TYPE;

/** Responce status*/
typedef enum {
	DHSTATUS_ERROR = 0,	///< Send Error string
	DHSTATUS_OK			///< Send OK string
} RESPONCE_STATUS;

/** Data for command result. */
typedef union {
	unsigned int id;	///< DeviceHive command id
	void *arg;			///< Anything else
} CommandResultArgument;

/** Function prototype for handling command result. */
typedef void (*command_callback)(CommandResultArgument data,
		RESPONCE_STATUS status, REQUEST_DATA_TYPE data_type, ...);

/** Struct for declaring callback for command result. */
typedef struct {
	command_callback callback;	///< Callback function.
	CommandResultArgument data;	///< Arguments for callback.
} COMMAND_RESULT;

/** Special struct for handling gpio result. */
typedef struct {
	unsigned int caused;	///< Which pins caused interruption.
	unsigned int state;		///< Current pins state.
	unsigned int timestamp;	///< Current internal time in microseconds.
} GPIO_DATA;

/** Struct for storing command result. */
typedef union {
	char array[INTERFACES_BUF_SIZE];	///< Raw data.
	const char *string;					///< Static string pointer.
	float adc;							///< Float value.
	GPIO_DATA gpio;						///< GPIO data.
} SENDERDATA;

/**
 *	\brief						Parse va list to SENDERDATA.
 *	\param[in]		ap			Std va list.
 *	\param[in,out]	data_type	Type of data in va. Can be modified on error.
 *	\param[out]		data		Pointer where data will be stored.
 *	\param[out]		data_len	Pointer where length of data will be stored.
 *	\param[out]		pin			Pointer for storing pin number.
 */
void dhsender_data_parse_va(va_list ap, REQUEST_DATA_TYPE *data_type,
		SENDERDATA *data, unsigned int *data_len, unsigned int *pin);

/**
 *	\brief							Convert SENDERDATA to json.
 *	\param[out]		buf				Buffer for output json.
 *	\param[in]		buf_len			Available buffer length.
 *	\param[in]		is_notification	Json for notifition will be generated on non zero value.
 *	\param[in]		data_type		Type of data.
 *	\param[in]		data			Pointer to SENDERDATA.
 *	\param[in]		data_len		Data length.
 *	\param[in]		pin				Pin number.
 *	\return 						Number of copied bytes, negative value on error.
 */
int dhsender_data_to_json(char *buf, unsigned int buf_len, int is_notification,
		REQUEST_DATA_TYPE data_type, SENDERDATA *data, unsigned int data_len,
		unsigned int pin);

#endif /* _DHSENDER_DATA_H_ */
