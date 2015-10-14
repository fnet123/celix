/*
 * Licensed under Apache License v2. See LICENSE for more information.
 */
#include "json_rpc.h"
#include "json_serializer.h"
#include "dyn_type.h"
#include "dyn_interface.h"
#include <jansson.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>


static int OK = 0;
static int ERROR = 1;

DFI_SETUP_LOG(jsonRpc);

typedef void (*gen_func_type)(void);

struct generic_service_layout {
    void *handle;
    gen_func_type methods[];
};

int jsonRpc_call(dyn_interface_type *intf, void *service, const char *request, char **out) {
    int status = OK;

    LOG_DEBUG("Parsing data: %s\n", request);
    json_error_t error;
    json_t *js_request = json_loads(request, 0, &error);
    json_t *arguments = NULL;
    const char *sig;
    if (js_request) {
        if (json_unpack(js_request, "{s:s}", "m", &sig) != 0) {
            LOG_ERROR("Got json error '%s'\n", error.text);
        } else {
            arguments = json_object_get(js_request, "a");
        }
    } else {
        LOG_ERROR("Got json error '%s' for '%s'\n", error.text, request);
        return 0;
    }

    LOG_DEBUG("Looking for method %s\n", sig);
    struct methods_head *methods = NULL;
    dynInterface_methods(intf, &methods);
    struct method_entry *entry = NULL;
    struct method_entry *method = NULL;
    TAILQ_FOREACH(entry, methods, entries) {
        if (strcmp(sig, entry->id) == 0) {
            method = entry;
            break;
        }
    }

    if (method == NULL) {
        status = ERROR;
        LOG_ERROR("Cannot find method with sig '%s'", sig);
    } else {
        LOG_DEBUG("RSA: found method '%s'\n", entry->id);
    }

    dyn_type *returnType = dynFunction_returnType(method->dynFunc);

    void (*fp)(void) = NULL;
    void *handle = NULL;
    if (status == OK) {
        struct generic_service_layout *serv = service;
        handle = serv->handle;
        fp = serv->methods[method->index];
    }

    dyn_function_type *func = NULL;
    int nrOfArgs = 0;
    if (status == OK) {
        nrOfArgs = dynFunction_nrOfArguments(entry->dynFunc);
        func = entry->dynFunc;
    }

    void *args[nrOfArgs];

    json_t *value = NULL;

    int i;
    int index = 0;

    void *ptr = NULL;
    void *ptrToPtr = &ptr;

    for (i = 0; i < nrOfArgs; i += 1) {
        dyn_type *argType = dynFunction_argumentTypeForIndex(func, i);
        enum dyn_function_argument_meta  meta = dynFunction_argumentMetaForIndex(func, i);
        if (meta == DYN_FUNCTION_ARGUMENT_META__STD) {
            value = json_array_get(arguments, index++);
            status = jsonSerializer_deserializeJson(argType, value, &(args[i]));
        } else if (meta == DYN_FUNCTION_ARGUMENT_META__PRE_ALLOCATED_OUTPUT) {
            dynType_alloc(argType, &args[i]);
        } else if (meta == DYN_FUNCTION_ARGUMENT_META__OUTPUT) {
            args[i] = &ptrToPtr;
        } else if (meta == DYN_FUNCTION_ARGUMENT_META__HANDLE) {
            args[i] = &handle;
        }

        if (status != OK) {
            break;
        }
    }
    json_decref(js_request);

    if (dynType_descriptorType(returnType) != 'N') {
        //NOTE To be able to handle exception only N as returnType is supported
        LOG_ERROR("Only interface methods with a native int are supported. Found type '%c'", (char)dynType_descriptorType(returnType));
        status = ERROR;
    }

    ffi_sarg returnVal;

    if (status == OK) {
        dynFunction_call(func, fp, (void *) &returnVal, args);
    }

    int funcCallStatus = (int)returnVal;
    if (funcCallStatus != 0) {
        LOG_WARNING("Error calling remote endpoint function, got error code %i", funcCallStatus);
    }

    json_t *jsonResult = NULL;
    for(i = 0; i < nrOfArgs; i += 1) {
        dyn_type *argType = dynFunction_argumentTypeForIndex(func, i);
        enum dyn_function_argument_meta  meta = dynFunction_argumentMetaForIndex(func, i);
        if (meta == DYN_FUNCTION_ARGUMENT_META__STD) {
            dynType_free(argType, args[i]);
        }
    }

    if (funcCallStatus == 0 && status == OK) {
        for (i = 0; i < nrOfArgs; i += 1) {
            dyn_type *argType = dynFunction_argumentTypeForIndex(func, i);
            enum dyn_function_argument_meta  meta = dynFunction_argumentMetaForIndex(func, i);
            if (meta == DYN_FUNCTION_ARGUMENT_META__PRE_ALLOCATED_OUTPUT) {
                if (status == OK) {
                    status = jsonSerializer_serializeJson(argType, args[i], &jsonResult);
                }
                dynType_free(argType, args[i]);
            } else if (meta == DYN_FUNCTION_ARGUMENT_META__OUTPUT) {
                if (ptr != NULL) {

                    dyn_type *typedType = NULL;
                    if (status == OK) {
                        status = dynType_typedPointer_getTypedType(argType, &typedType);
                    }

                    dyn_type *typedTypedType = NULL;
                    if (status == OK) {
                        status = dynType_typedPointer_getTypedType(typedType, &typedTypedType);
                    }

                    status = jsonSerializer_serializeJson(typedTypedType, ptr, &jsonResult);

                    if (status == OK) {
                        dynType_free(typedTypedType, ptr);
                    }
                } else {
                    LOG_DEBUG("Output ptr is null");
                }
            }

            if (status != OK) {
                break;
            }
        }
    }

    char *response = NULL;
    if (status == OK) {
        LOG_DEBUG("creating payload\n");
        json_t *payload = json_object();
        if (funcCallStatus == 0) {
            if (jsonResult == NULL) {
                //ignore -> no result
            } else {
                LOG_DEBUG("Setting result payload");
                json_object_set_new(payload, "r", jsonResult);
            }
        } else {
            LOG_DEBUG("Setting error payload");
            json_object_set_new(payload, "e", json_integer(funcCallStatus));
        }
        response = json_dumps(payload, JSON_DECODE_ANY);
        json_decref(payload);
        LOG_DEBUG("status ptr is %p. response is '%s'\n", status, response);
    }

    if (status == OK) {
        *out = response;
    } else {
        free(response);
    }

    return status;
}

int jsonRpc_prepareInvokeRequest(dyn_function_type *func, const char *id, void *args[], char **out) {
    int status = OK;


    LOG_DEBUG("Calling remote function '%s'\n", id);
    json_t *invoke = json_object();
    json_object_set_new(invoke, "m", json_string(id));

    json_t *arguments = json_array();
    json_object_set_new(invoke, "a", arguments);

    int i;
    int nrOfArgs = dynFunction_nrOfArguments(func);
    for (i = 0; i < nrOfArgs; i +=1) {
        dyn_type *type = dynFunction_argumentTypeForIndex(func, i);
        enum dyn_function_argument_meta  meta = dynFunction_argumentMetaForIndex(func, i);
        if (meta == DYN_FUNCTION_ARGUMENT_META__STD) {
            json_t *val = NULL;

            int rc = jsonSerializer_serializeJson(type, args[i], &val);
            if (rc == 0) {
                json_array_append_new(arguments, val);
            } else {
                status = ERROR;
                break;
            }
        } else {
            //skip handle / output types
        }
    }

    char *invokeStr = json_dumps(invoke, JSON_DECODE_ANY);
    json_decref(invoke);

    if (status == OK) {
        *out = invokeStr;
    }

    return status;
}

int jsonRpc_handleReply(dyn_function_type *func, const char *reply, void *args[]) {
    int status = OK;

    json_error_t error;
    json_t *replyJson = json_loads(reply, JSON_DECODE_ANY, &error);
    if (replyJson == NULL) {
        status = ERROR;
        LOG_ERROR("Error parsing json '%s', got error '%s'", reply, error.text);
    }

    json_t *result = NULL;
    if (status == OK) {
        result = json_object_get(replyJson, "r"); //TODO check
        if (result == NULL) {
            status = ERROR;
            LOG_ERROR("Cannot find r entry in json reply '%s'", reply);
        }
    }

    if (status == OK) {
        int nrOfArgs = dynFunction_nrOfArguments(func);
        int i;
        for (i = 0; i < nrOfArgs; i += 1) {
            dyn_type *argType = dynFunction_argumentTypeForIndex(func, i);
            enum dyn_function_argument_meta meta = dynFunction_argumentMetaForIndex(func, i);
            if (meta == DYN_FUNCTION_ARGUMENT_META__PRE_ALLOCATED_OUTPUT) {
                //FIXME need a tmp because deserialize does always does a create (add option?)
                dyn_type *subType = NULL;
                dynType_typedPointer_getTypedType(argType, &subType);
                void *tmp = NULL;
                size_t size = dynType_size(subType);
                status = jsonSerializer_deserializeJson(subType, result, &tmp);
                void **out = (void **)args[i];
                memcpy(*out, tmp, size);
                dynType_free(subType, tmp);
            } else if (meta == DYN_FUNCTION_ARGUMENT_META__OUTPUT) {
                dyn_type *subType = NULL;
                dynType_typedPointer_getTypedType(argType, &subType);
                dyn_type *subSubType = NULL;
                dynType_typedPointer_getTypedType(subType, &subSubType);
                void **out = (void **)args[i];
                status = jsonSerializer_deserializeJson(subSubType, result, *out);
            } else {
                //skip
            }
        }
    }

    json_decref(replyJson);

    return status;
}