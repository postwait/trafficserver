/** @file

  Transforms content using gzip

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <assert.h>
#include <ts/ts.h>

#define DICT_PATH_MAX 512
#define DICT_ENTRY_MAX 2048

typedef struct
{
  TSHttpTxn txn;
  TSVIO output_vio;
  TSIOBuffer output_buffer;
  TSIOBufferReader output_reader;
  int output_length;
  z_stream zstrm;
  uLong crc;
  int state;
  int flag;
} GzipData;

char preload_file[1024];
uLong dictId;
int preload = 0;
char dictionary[800000];


void
load_dictionary(char *dict, uLong * adler)
{
  FILE *fp;
  int i = 0;

  fp = fopen(preload_file, "r");
  if (!fp) {
    TSError("gunzip-transform: ERROR: Unable to open dict file %s\n", preload_file);
    exit(0);
  }

  i = 0;
  while (!feof(fp)) {
    if (fscanf(fp, "%s\n", dict + i) == 1) {
      i = strlen(dict);
      TSstrlcat(dict + i, " ", sizeof(dict) - i);
      i++;
    }
  }
  dict[i - 1] = '\0';

  /* TODO figure out the right way to do the adler -- this one is all messed up */
  *adler = adler32(*adler, (Byte *) dict, strlen(dict));
}

static voidpf
gzip_alloc(voidpf opaque, uInt items, uInt size)
{
  return (voidpf) TSmalloc(items * size);
}


static void
gzip_free(voidpf opaque, voidpf address)
{
  TSfree(address);
}


static GzipData *
gzip_data_alloc()
{
  GzipData *data;

  data = (GzipData *) TSmalloc(sizeof(GzipData));
  data->output_vio = NULL;
  data->output_buffer = NULL;
  data->output_reader = NULL;
  data->output_length = 0;
  data->state = 0;
  data->flag = 1;
  data->crc = crc32(0L, Z_NULL, 0);

  data->zstrm.next_in = Z_NULL;
  data->zstrm.avail_in = 0;
  data->zstrm.total_in = 0;
  data->zstrm.next_out = Z_NULL;
  data->zstrm.avail_out = 0;
  data->zstrm.total_out = 0;
  data->zstrm.zalloc = gzip_alloc;
  data->zstrm.zfree = gzip_free;
  data->zstrm.opaque = (voidpf) 0;      /* Z_NULL */
  data->zstrm.data_type = Z_ASCII;

  dictId = adler32(0L, Z_NULL, 0);

  return data;
}


static void
gzip_data_destroy(GzipData * data)
{
  int err;

  if (data) {
    err = inflateEnd(&data->zstrm);
    if (err != Z_OK) {
      TSError("gunzip-transform: ERROR: inflateEnd (%d)!", err);
    }

    if (data->output_buffer)
      TSIOBufferDestroy(data->output_buffer);
    TSfree(data);
  }
}


static void
gzip_transform_init(TSCont contp, GzipData * data)
{
  TSVConn output_conn;

  data->state = 1;

  /* Get the output connection where we'll write data to. */
  output_conn = TSTransformOutputVConnGet(contp);

  data->output_buffer = TSIOBufferCreate();
  data->output_reader = TSIOBufferReaderAlloc(data->output_buffer);
  data->output_vio = TSVConnWrite(output_conn, contp, data->output_reader, INT64_MAX);
}


static void
gzip_transform_one(GzipData * data, TSIOBufferReader input_reader, int amount)
{
  TSIOBufferBlock blkp;
  const char *ibuf;
  char *obuf;
  int64_t ilength, olength;
  int err = Z_OK;

  while (amount > 0) {
    blkp = TSIOBufferReaderStart(input_reader);
    ibuf = TSIOBufferBlockReadStart(blkp, input_reader, &ilength);

    if (ilength > amount) {
      ilength = amount;
    }

    data->zstrm.next_in = (unsigned char *) ibuf;
    data->zstrm.avail_in = ilength;

    if (data->flag) {
      err = inflateInit(&data->zstrm);
      data->flag = 0;
      if (err != Z_OK) {
        TSError("gunzip-transform: ERROR: inflateInit (%d)!", err);
        exit(1);
      }
    }

    while (data->zstrm.avail_in > 0 && err != Z_STREAM_END) {
      blkp = TSIOBufferStart(data->output_buffer);

      obuf = TSIOBufferBlockWriteStart(blkp, &olength);

      data->zstrm.next_out = (unsigned char *) obuf;
      data->zstrm.avail_out = olength;

      /* Uncompress */
      err = inflate(&data->zstrm, Z_NO_FLUSH);
/*
	    assert( (err == Z_OK) || (err = Z_STREAM_END) || (err = Z_NEED_DICT));
*/
      if (err == Z_NEED_DICT) {
        assert(preload);
        TSDebug("gunzip-transform", "Transform needs dictionary");
        /* TODO assert encoding dict is same as the decoding one, else send an error page */
        /* Dont send the user binary junk! */

        err = inflateSetDictionary(&data->zstrm, (Bytef *) dictionary, strlen(dictionary));
        if (err != Z_OK) {
          TSError("gunzip-transform: ERROR: inflateSetDictionary (%d)!", err);
        }
      }

      if (olength > data->zstrm.avail_out) {
        TSIOBufferProduce(data->output_buffer, olength - data->zstrm.avail_out);
        data->output_length += (olength - data->zstrm.avail_out);
      }

    }

    /* TODO compute crc */

    TSIOBufferReaderConsume(input_reader, ilength);
    amount -= ilength;
  }
}


static void
gzip_transform_finish(GzipData * data)
{
  if (data->state == 1) {
    TSIOBufferBlock blkp;
    char *obuf;
    int64_t olength;
    int err;

    data->state = 2;

    for (;;) {
      blkp = TSIOBufferStart(data->output_buffer);

      obuf = TSIOBufferBlockWriteStart(blkp, &olength);
      data->zstrm.next_out = (unsigned char *) obuf;
      data->zstrm.avail_out = olength;

      /* Uncompress remaining data */
      err = inflate(&data->zstrm, Z_FINISH);

      if (olength > data->zstrm.avail_out) {
        TSIOBufferProduce(data->output_buffer, olength - data->zstrm.avail_out);
        data->output_length += (olength - data->zstrm.avail_out);
      }

      if (err == Z_OK) {        /* more to uncompress */
        continue;
      }
      /* done! */
      break;
    }

    if (data->output_length != (data->zstrm.total_out)) {
      TSError("gunzip-transform: ERROR: output lengths don't match (%d, %ld)", data->output_length,
               data->zstrm.total_out);
    }

    /* TODO crc checks */
  }
}


static void
gzip_transform_do(TSCont contp)
{
  TSVIO write_vio;
  GzipData *data;
  int towrite;
  int avail;
  int length;

  /* Get our data structure for this operation. The private data
     structure contains the output vio and output buffer. If the
     private data structure pointer is NULL, then we'll create it
     and initialize its internals. */
  data = TSContDataGet(contp);
  if (data->state == 0) {
    gzip_transform_init(contp, data);
  }

  /* Get the write vio for the write operation that was performed on
     ourself. This vio contains the buffer that we are to read from
     as well as the continuation we are to call when the buffer is
     empty. */
  write_vio = TSVConnWriteVIOGet(contp);
  length = data->output_length;

  /* We also check to see if the write vio's buffer is non-NULL. A
     NULL buffer indicates that the write operation has been
     shutdown and that the continuation does not want us to send any
     more WRITE_READY or WRITE_COMPLETE events. For this simplistic
     transformation that means we're done. In a more complex
     transformation we might have to finish writing the transformed
     data to our output connection. */
  if (!TSVIOBufferGet(write_vio)) {

    gzip_transform_finish(data);

    TSVIONBytesSet(data->output_vio, data->output_length);

    TSVIOReenable(data->output_vio);
    return;
  }

  /* Determine how much data we have left to read. For this gzip
     transform plugin this is also the amount of data we have left
     to write to the output connection. */
  towrite = TSVIONTodoGet(write_vio);
  if (towrite > 0) {
    /* The amount of data left to read needs to be truncated by
       the amount of data actually in the read buffer. */
    avail = TSIOBufferReaderAvail(TSVIOReaderGet(write_vio));
    if (towrite > avail) {
      towrite = avail;
    }

    if (towrite > 0) {
      gzip_transform_one(data, TSVIOReaderGet(write_vio), towrite);

      /* Modify the write vio to reflect how much data we've
         completed. */
      TSVIONDoneSet(write_vio, TSVIONDoneGet(write_vio) + towrite);
    }
  }

  /* Now we check the write vio to see if there is data left to
     read. */
  if (TSVIONTodoGet(write_vio) > 0) {
    if (towrite > 0) {
      /* If we output some data then we reenable the output
         connection by reenabling the output vio. This will wakeup
         the output connection and allow it to consume data from the
         output buffer. */
      if (data->output_length > length) {
        TSVIOReenable(data->output_vio);
      }

      /* Call back the write vio continuation to let it know that we
         are ready for more data. */
      TSContCall(TSVIOContGet(write_vio), TS_EVENT_VCONN_WRITE_READY, write_vio);
    }
  } else {
    /* If there is no data left to read, then we modify the output
       vio to reflect how much data the output connection should
       expect. This allows the output connection to know when it
       is done reading. We then reenable the output connection so
       that it can consume the data we just gave it. */
    gzip_transform_finish(data);

    TSVIONBytesSet(data->output_vio, data->output_length);

    if (data->output_length > length) {
      TSVIOReenable(data->output_vio);
    }

    /* Call back the write vio continuation to let it know that we
       have completed the write operation. */
    TSContCall(TSVIOContGet(write_vio), TS_EVENT_VCONN_WRITE_COMPLETE, write_vio);
  }
}


static int
gzip_transform(TSCont contp, TSEvent event, void *edata)
{
  /* Check to see if the transformation has been closed by a call to
     TSVConnClose. */
  if (TSVConnClosedGet(contp)) {
    gzip_data_destroy(TSContDataGet(contp));
    TSContDestroy(contp);
    return 0;
  } else {
    switch (event) {
    case TS_EVENT_ERROR:
      {
        TSVIO write_vio;

        /* Get the write vio for the write operation that was
           performed on ourself. This vio contains the continuation of
           our parent transformation. */
        write_vio = TSVConnWriteVIOGet(contp);

        /* Call back the write vio continuation to let it know that we
           have completed the write operation. */
        TSContCall(TSVIOContGet(write_vio), TS_EVENT_ERROR, write_vio);
      }
      break;
    case TS_EVENT_VCONN_WRITE_COMPLETE:
    case TS_EVENT_VCONN_EOS:
      /* When our output connection says that it has finished
         reading all the data we've written to it then we should
         shutdown the write portion of its connection to
         indicate that we don't want to hear about it anymore. */
      TSVConnShutdown(TSTransformOutputVConnGet(contp), 0, 1);
      break;
    case TS_EVENT_VCONN_WRITE_READY:
    default:
      /* If we get a WRITE_READY event or any other type of
         event (sent, perhaps, because we were reenabled) then
         we'll attempt to transform more data. */
      gzip_transform_do(contp);
      break;
    }
  }

  return 0;
}

static int
gzip_transformable(TSHttpTxn txnp, int server)
{
  TSMBuffer bufp;
  TSMLoc hdr_loc;
  TSMLoc field_loc;
  const char *value;

  if (server) {
    TSHttpTxnServerRespGet(txnp, &bufp, &hdr_loc);
  } else {
    TSHttpTxnCachedRespGet(txnp, &bufp, &hdr_loc);
  }

  /* We only want to do gunzip(inflate) on documents that have a
     content-encoding "deflate". */

  field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, "Content-Encoding", -1);
  if (!field_loc) {
    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
    return -4;
  }

  value = TSMimeHdrFieldValueStringGet(bufp, hdr_loc, field_loc, 0, NULL);
  if (value && (strncasecmp(value, "deflate", sizeof("deflate") - 1) == 0)) {
    TSHandleMLocRelease(bufp, hdr_loc, field_loc);
    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
    return 0;
  } else {
    TSHandleMLocRelease(bufp, hdr_loc, field_loc);
    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
    return -5;
  }
}


static void
gzip_transform_add(TSHttpTxn txnp, int flag)
{
  TSVConn connp;
  GzipData *data;

  data = gzip_data_alloc();
  data->txn = txnp;

  connp = TSTransformCreate(gzip_transform, txnp);
  TSContDataSet(connp, data);
  TSHttpTxnHookAdd(txnp, TS_HTTP_RESPONSE_TRANSFORM_HOOK, connp);
}


static int
transform_plugin(TSCont contp, TSEvent event, void *edata)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;
  int reason;

  switch (event) {
  case TS_EVENT_HTTP_READ_REQUEST_HDR:
    {
      TSMBuffer bufp;
      TSMLoc hdr_loc;
      TSMLoc ae_loc;           /* for the accept encoding mime field */

      TSHttpTxnClientReqGet(txnp, &bufp, &hdr_loc);
      TSMimeHdrFieldCreate(bufp, hdr_loc, &ae_loc); /* Probably should check for errors */
      TSMimeHdrFieldNameSet(bufp, hdr_loc, ae_loc, "Accept-Encoding", -1);
      TSMimeHdrFieldValueAppend(bufp, hdr_loc, ae_loc, -1, "deflate", -1);
      TSMimeHdrFieldAppend(bufp, hdr_loc, ae_loc);
      TSHandleMLocRelease(bufp, hdr_loc, ae_loc);
      TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);

      TSDebug("gunzip-transform", "Changed request header to accept deflate encoding");
      TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
      break;
    }
  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    reason = gzip_transformable(txnp, 1);
    if (reason >= 0) {
      TSMBuffer bufp;
      TSMLoc hdr_loc;
      TSMLoc field_loc;

      TSHttpTxnServerRespGet(txnp, &bufp, &hdr_loc);
      field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, "Content-Encoding", -1);
      TSMimeHdrFieldDestroy(bufp, hdr_loc, field_loc);
      TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);

      TSDebug("gunzip-transform", "server content transformable");
      gzip_transform_add(txnp, 1);
    } else {
      TSDebug("gunzip-transform", "server content NOT transformable [%d]", reason);
    }

    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    break;

  case TS_EVENT_HTTP_READ_CACHE_HDR:

    TSDebug("gunzip-transform", "Cached data");

    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    break;

  default:
    exit(1);
  }

  return 0;
}


void
TSPluginInit(int argc, const char *argv[])
{
  if (argc == 2) {
    TSstrlcpy(preload_file, argv[1], sizeof(preload_file));
    preload = 1;
    load_dictionary(dictionary, &dictId);
  }

  TSHttpHookAdd(TS_HTTP_READ_REQUEST_HDR_HOOK, TSContCreate(transform_plugin, NULL));
  TSHttpHookAdd(TS_HTTP_READ_RESPONSE_HDR_HOOK, TSContCreate(transform_plugin, NULL));
  TSHttpHookAdd(TS_HTTP_READ_CACHE_HDR_HOOK, TSContCreate(transform_plugin, NULL));
}
