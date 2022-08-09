/*
 * Copyright 2008-2015 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */

#include "mrcp_recog_engine.h"
#include "mpf_activity_detector.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include "vosk_api.h"

#include "stdlib.h"


#define RECOG_ENGINE_TASK_NAME "Vosk Recog Engine"


typedef struct vosk_recog_engine_t vosk_recog_engine_t;
typedef struct vosk_recog_channel_t vosk_recog_channel_t;
typedef struct vosk_recog_msg_t vosk_recog_msg_t;

/** Declaration of recognizer engine methods */
static apt_bool_t vosk_recog_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t vosk_recog_engine_open(mrcp_engine_t *engine);
static apt_bool_t vosk_recog_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* vosk_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	vosk_recog_engine_destroy,
	vosk_recog_engine_open,
	vosk_recog_engine_close,
	vosk_recog_engine_channel_create
};

/** Declaration of recognizer channel methods */
static apt_bool_t vosk_recog_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t vosk_recog_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t vosk_recog_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t vosk_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	vosk_recog_channel_destroy,
	vosk_recog_channel_open,
	vosk_recog_channel_close,
	vosk_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t vosk_recog_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t vosk_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t vosk_recog_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t vosk_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	vosk_recog_stream_destroy,
	NULL,
	NULL,
	NULL,
	vosk_recog_stream_open,
	vosk_recog_stream_close,
	vosk_recog_stream_write,
	NULL
};

static apt_bool_t vosk_recog_recognition_complete(vosk_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause);


/** Declaration of kaldi recognizer engine */
struct vosk_recog_engine_t {
	apt_consumer_task_t    *task;
	VoskModel              *model;
};

/** Declaration of kaldi recognizer channel */
struct vosk_recog_channel_t {
	/** Back pointer to engine */
	vosk_recog_engine_t     *kaldi_engine;
	/** Engine channel base */
	mrcp_engine_channel_t   *channel;

	/** Active (in-progress) recognition request */
	mrcp_message_t          *recog_request;
	/** Pending stop response */
	mrcp_message_t          *stop_response;
	/** Indicates whether input timers are started */
	apt_bool_t               timers_started;
	/** Voice activity detector */
	mpf_activity_detector_t *detector;
	/** File to write utterance to */
	FILE                    *audio_out;
	/** Actual recognizer **/
	VoskRecognizer          *recognizer;

	/** Start input sent flag **/
	apt_bool_t				start_input_msg_sent;

	/** dtmf buffer to store **/
	 const char 			*dtmf_buffer;

	 /** Inactivity timer  */
	 apt_timer_t       *dtmf_interdigit_timeout_timer;
	 apt_timer_queue_t		*dtmf_interdigit_timeout_timer_queue;


};

typedef enum {
	vosk_recog_MSG_OPEN_CHANNEL,
	vosk_recog_MSG_CLOSE_CHANNEL,
	vosk_recog_MSG_REQUEST_PROCESS
} vosk_recog_msg_type_e;

/** Declaration of kaldi recognizer task message */
struct vosk_recog_msg_t {
	vosk_recog_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};

static apt_bool_t vosk_recog_msg_signal(vosk_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t vosk_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="RECOG-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN,"RECOG-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK   APT_LOG_MARK_DECLARE(RECOG_PLUGIN)

/** Create kaldi recognizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	vosk_recog_engine_t *kaldi_engine = (vosk_recog_engine_t*)apr_palloc(pool,sizeof(vosk_recog_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(vosk_recog_msg_t),pool);
	kaldi_engine->task = apt_consumer_task_create(kaldi_engine,msg_pool,pool);
	if(!kaldi_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(kaldi_engine->task);
	apt_task_name_set(task,RECOG_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = vosk_recog_msg_process;
	}

	kaldi_engine->model = vosk_model_new("/opt/kaldi/model");

	/* create engine base */
	return mrcp_engine_create(
				MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
				kaldi_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy recognizer engine */
static apt_bool_t vosk_recog_engine_destroy(mrcp_engine_t *engine)
{
	vosk_recog_engine_t *kaldi_engine = (vosk_recog_engine_t*)engine->obj;
	if(kaldi_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(kaldi_engine->task);
		apt_task_destroy(task);
		kaldi_engine->task = NULL;
	}
	if (kaldi_engine->model) {
		vosk_model_free(kaldi_engine->model);
		kaldi_engine->model = NULL;
	}
	return TRUE;
}

/** Open recognizer engine */
static apt_bool_t vosk_recog_engine_open(mrcp_engine_t *engine)
{
	vosk_recog_engine_t *kaldi_engine = (vosk_recog_engine_t*)engine->obj;
	if(kaldi_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(kaldi_engine->task);
		apt_task_start(task);
	}
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close recognizer engine */
static apt_bool_t vosk_recog_engine_close(mrcp_engine_t *engine)
{
	vosk_recog_engine_t *kaldi_engine = (vosk_recog_engine_t*)engine->obj;
	if(kaldi_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(kaldi_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t* vosk_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create kaldi recog channel */
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)apr_palloc(pool,sizeof(vosk_recog_channel_t));
	recog_channel->kaldi_engine = (vosk_recog_engine_t*)engine->obj;
        recog_channel->recognizer = NULL;
	recog_channel->recog_request = NULL;
	recog_channel->stop_response = NULL;
	recog_channel->detector = mpf_activity_detector_create(pool);
	recog_channel->audio_out = NULL;

	capabilities = mpf_sink_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			recog_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	recog_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			recog_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	return recog_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t vosk_recog_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destrtoy */
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t vosk_recog_channel_open(mrcp_engine_channel_t *channel)
{
	return vosk_recog_msg_signal(vosk_recog_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t vosk_recog_channel_close(mrcp_engine_channel_t *channel)
{
	return vosk_recog_msg_signal(vosk_recog_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t vosk_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return vosk_recog_msg_signal(vosk_recog_MSG_REQUEST_PROCESS,channel,request);
}

/* Timer callback */
static void vosk_recog_channel_interdigit_timeout_timer_proc(apt_timer_t *timer, void *obj)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Inside Timer Call back function");
	vosk_recog_channel_t *recog_channel = obj;
	if(!recog_channel) return;

	if(recog_channel->dtmf_interdigit_timeout_timer == timer) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Interdigit Timeout Triggerred");
		vosk_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
	}
}


/** Process RECOGNIZE request */
static apt_bool_t vosk_recog_channel_recognize(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process RECOGNIZE request */
	mrcp_recog_header_t *recog_header;
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_sink_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	recog_channel->timers_started = TRUE;

	/* get recognizer header */
	recog_header = (mrcp_recog_header_t*)mrcp_resource_header_get(request);
	if(recog_header) {
		//if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_START_INPUT_TIMERS) == TRUE) {
		//	recog_channel->timers_started = recog_header->start_input_timers;
		//}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) == TRUE) {
			mpf_activity_detector_noinput_timeout_set(recog_channel->detector,recog_header->no_input_timeout);
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE) {
			mpf_activity_detector_silence_timeout_set(recog_channel->detector,recog_header->speech_complete_timeout);
		}
	}

	if(!recog_channel->audio_out) {
		const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
		char *file_name = apr_psprintf(channel->pool,"utter-%dkHz-%s.pcm",
							descriptor->sampling_rate/1000,
							request->channel_id.session_id.buf);
		char *file_path = apt_vardir_filepath_get(dir_layout,file_name,channel->pool);
		if(file_path) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Open Utterance Output File [%s] for Writing",file_path);
			recog_channel->audio_out = fopen(file_path,"wb");
			if(!recog_channel->audio_out) {
				apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Open Utterance Output File [%s] for Writing",file_path);
			}
		}

	}
	if(!recog_channel->recognizer) {
		vosk_recog_engine_t *kaldi_engine = recog_channel->kaldi_engine;
		recog_channel->recognizer = vosk_recognizer_new(kaldi_engine->model, 8000.0f);
		vosk_recognizer_set_max_alternatives(recog_channel->recognizer, 5);
		vosk_recognizer_set_nlsml(recog_channel->recognizer, 1);
	}

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	recog_channel->recog_request = request;
	recog_channel->start_input_msg_sent = FALSE;
	recog_channel->dtmf_buffer = "";
	recog_channel->dtmf_interdigit_timeout_timer_queue = NULL;
	recog_channel->dtmf_interdigit_timeout_timer_queue = apt_timer_queue_create(channel->pool);
	recog_channel->dtmf_interdigit_timeout_timer = NULL;
	recog_channel->dtmf_interdigit_timeout_timer = apt_timer_create(
													recog_channel->dtmf_interdigit_timeout_timer_queue,
													vosk_recog_channel_interdigit_timeout_timer_proc,
													recog_channel,
													channel->pool);
	if (recog_channel->dtmf_interdigit_timeout_timer)
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Successfully Created interdigit timeout timer-2 [0x%x]", recog_channel->dtmf_interdigit_timeout_timer);


	return TRUE;
}

/** Process STOP request */
static apt_bool_t vosk_recog_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process STOP request */
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)channel->method_obj;
	/* store STOP request, make sure there is no more activity and only then send the response */
	recog_channel->stop_response = response;
	return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t vosk_recog_channel_timers_start(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)channel->method_obj;
	recog_channel->timers_started = TRUE;
	return mrcp_engine_channel_message_send(channel,response);
}

/** Dispatch MRCP request */
static apt_bool_t vosk_recog_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case RECOGNIZER_SET_PARAMS:
			break;
		case RECOGNIZER_GET_PARAMS:
			break;
		case RECOGNIZER_DEFINE_GRAMMAR:
			break;
		case RECOGNIZER_RECOGNIZE:
			processed = vosk_recog_channel_recognize(channel,request,response);
			break;
		case RECOGNIZER_GET_RESULT:
			break;
		case RECOGNIZER_START_INPUT_TIMERS:
			processed = vosk_recog_channel_timers_start(channel,request,response);
			break;
		case RECOGNIZER_STOP:
			processed = vosk_recog_channel_stop(channel,request,response);
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t vosk_recog_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t vosk_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t vosk_recog_stream_close(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/* Raise kaldi START-OF-INPUT event */
static apt_bool_t vosk_recog_start_of_input(vosk_recog_channel_t *recog_channel)
{
	/* create START-OF-INPUT event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_START_OF_INPUT,
						recog_channel->recog_request->pool);
	if(!message) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Unable to create message for start_of_input"); 
		return FALSE;
	}

	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

static const char* vosk_recog_create_dtmf_body_response(vosk_recog_channel_t *recog_channel)
{

	char* dtmf_body = apr_psprintf(recog_channel->channel->pool,"<?xml version=\"1.0\"?>\n<result grammar=\"default\">\n\t<interpretation grammar=\"default\" confidence=\"100.0\">\n\t\t<input mode=\"dtmf\">%s</input>\n\t\t<instance>%s</instance>\n\t</interpretation>\n</result>",recog_channel->dtmf_buffer,recog_channel->dtmf_buffer);
	return dtmf_body;
}


/* Raise kaldi RECOGNITION-COMPLETE event */
static apt_bool_t vosk_recog_recognition_complete(vosk_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause)
{
	mrcp_recog_header_t *recog_header;
	/* create RECOGNITION-COMPLETE event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_RECOGNITION_COMPLETE,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* get/allocate recognizer header */
	recog_header = (mrcp_recog_header_t*)mrcp_resource_header_prepare(message);
	if(recog_header) {
		/* set completion cause */
		if (strlen(recog_channel->dtmf_buffer) > 0)
			cause = RECOGNIZER_COMPLETION_CAUSE_SUCCESS;
		recog_header->completion_cause = cause;
		mrcp_resource_header_property_add(message,RECOGNIZER_HEADER_COMPLETION_CAUSE);
	}
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	if(cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS) {
		{
			if (strlen(recog_channel->dtmf_buffer) > 0){
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Dtmf data is %s ",recog_channel->dtmf_buffer);
				const char* dtmf_result = vosk_recog_create_dtmf_body_response(recog_channel);
				apt_string_assign_n(&message->body,dtmf_result,strlen(dtmf_result),message->pool);

			}
			else {
				const char *result = vosk_recognizer_result(recog_channel->recognizer);
				apt_string_assign_n(&message->body,result,strlen(result),message->pool);
			}
		}
		{
			/* get/allocate generic header */
			mrcp_generic_header_t *generic_header = mrcp_generic_header_prepare(message);
			if(generic_header) {
				/* set content types */
				apt_string_assign(&generic_header->content_type,"application/x-nlsml",message->pool);
				mrcp_generic_header_property_add(message,GENERIC_HEADER_CONTENT_TYPE);
			}
		}
	}

	recog_channel->recog_request = NULL;
	recog_channel->dtmf_buffer = NULL;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t vosk_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)stream->obj;
	if(recog_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(recog_channel->channel,recog_channel->stop_response);
		recog_channel->stop_response = NULL;
		recog_channel->recog_request = NULL;
		return TRUE;
	}

	if(recog_channel->recog_request) {
		mpf_detector_event_e det_event = mpf_activity_detector_process(recog_channel->detector,frame);
		switch(det_event) {
			case MPF_DETECTOR_EVENT_ACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Activity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				vosk_recog_start_of_input(recog_channel);
				break;
			case MPF_DETECTOR_EVENT_INACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Inactivity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				vosk_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
				break;
			case MPF_DETECTOR_EVENT_NOINPUT:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Noinput " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				if(recog_channel->timers_started == TRUE) {
					vosk_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
				}
				break;
			default:
				break;
		}

		if(recog_channel->recog_request) {
			if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
				if(frame->marker == MPF_MARKER_START_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Start of Event " APT_SIDRES_FMT " id:%d",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id);
					recog_channel->timers_started = TRUE;
					if (!recog_channel->start_input_msg_sent) {
						vosk_recog_start_of_input(recog_channel);
						recog_channel->start_input_msg_sent = TRUE;
					}

					// append the data to the dtmf string recognized
					switch (frame->event_frame.event_id) {
					case 0:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"0 " , NULL);
						break;
					case 1:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"1 " , NULL);
						break;
					case 2:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"2 " , NULL);
						break;
					case 3:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"3 " , NULL);
						break;
					case 4:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"4 " , NULL);
						break;
					case 5:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"5 " , NULL);
						break;
					case 6:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"6 " , NULL);
						break;
					case 7:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"7 " , NULL);
						break;
					case 8:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"8 " , NULL);
						break;
					case 9:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"9 " , NULL);
						break;
					case 10:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"* " , NULL);
						break;
					case 11:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"# " , NULL);
						break;
					case 12:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"A " , NULL);
						break;
					case 13:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"B " , NULL);
						break;
					case 14:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"C " , NULL);
						break;
					case 15:
						recog_channel->dtmf_buffer = apr_pstrcat(recog_channel->channel->pool, recog_channel->dtmf_buffer,"D " , NULL);
						break;

					}

					/* (re)set inactivity timer on every dtmf event received */
					if(recog_channel->dtmf_interdigit_timeout_timer) {
						apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Setting the timer for 3 seconds-2, 0x%x ", recog_channel->dtmf_interdigit_timeout_timer );
						apt_timer_set(recog_channel->dtmf_interdigit_timeout_timer,3000);
					}

				}
				else if(frame->marker == MPF_MARKER_END_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected End of Event " APT_SIDRES_FMT " id:%d duration:%d ts",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id,
						frame->event_frame.duration);
					//vosk_recog_append_dtmf_buffer_to_utterance(recog_channel, frame->event_frame.event_id);

				}
			}
		}

		if(recog_channel->audio_out) {
			fwrite(frame->codec_frame.buffer,1,frame->codec_frame.size,recog_channel->audio_out);
		}
		if(recog_channel->recognizer) {
			if (vosk_recognizer_accept_waveform(recog_channel->recognizer, (const char*)frame->codec_frame.buffer, frame->codec_frame.size)) {
				vosk_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
			}
		}
	}
	return TRUE;
}


static apt_bool_t vosk_recog_msg_signal(vosk_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	vosk_recog_channel_t *kaldi_channel = (vosk_recog_channel_t*)channel->method_obj;
	vosk_recog_engine_t *kaldi_engine = kaldi_channel->kaldi_engine;
	apt_task_t *task = apt_consumer_task_base_get(kaldi_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		vosk_recog_msg_t *kaldi_msg;
		msg->type = TASK_MSG_USER;
		kaldi_msg = (vosk_recog_msg_t*) msg->data;

		kaldi_msg->type = type;
		kaldi_msg->channel = channel;
		kaldi_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t vosk_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	vosk_recog_msg_t *kaldi_msg = (vosk_recog_msg_t*)msg->data;
	switch(kaldi_msg->type) {
		case vosk_recog_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(kaldi_msg->channel,TRUE);
			break;
		case vosk_recog_MSG_CLOSE_CHANNEL:
		{
			/* close channel, make sure there is no activity and send asynch response */
			vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)kaldi_msg->channel->method_obj;
			if(recog_channel->audio_out) {
				fclose(recog_channel->audio_out);
				recog_channel->audio_out = NULL;
			}
			if(recog_channel->recognizer) {
				vosk_recognizer_free(recog_channel->recognizer);
				recog_channel->recognizer = NULL;
				recog_channel->dtmf_buffer = NULL;
				recog_channel->dtmf_interdigit_timeout_timer = NULL;
			}

			mrcp_engine_channel_close_respond(kaldi_msg->channel);
			break;
		}
		case vosk_recog_MSG_REQUEST_PROCESS:
			vosk_recog_channel_request_dispatch(kaldi_msg->channel,kaldi_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}
