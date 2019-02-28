/*
  Copyright (c) 2013, Dan VerWeire<dverweire@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
  
  SQL_ERROR Value is: -1
  SQL_SUCCESS Value is: 0
  SQL_SQL_SUCCESS_WITH_INFO Value is: 1
  SQL_NEED_DATA Value is: 99

*/

#include <napi.h>
#include <uv.h>
#include <time.h>

#include "odbc.h"
#include "odbc_connection.h"
#include "odbc_statement.h"

Napi::FunctionReference ODBCStatement::constructor;

HENV ODBCStatement::hENV;
HDBC ODBCStatement::hDBC;

Napi::Object ODBCStatement::Init(Napi::Env env, Napi::Object exports) {

  DEBUG_PRINTF("ODBCStatement::Init\n");

  Napi::HandleScope scope(env);

  Napi::Function constructorFunction = DefineClass(env, "ODBCStatement", {
    InstanceMethod("prepare", &ODBCStatement::Prepare),
    InstanceMethod("bind", &ODBCStatement::Bind),
    InstanceMethod("execute", &ODBCStatement::Execute),
    InstanceMethod("close", &ODBCStatement::Close),
  });

  // Attach the Database Constructor to the target object
  constructor = Napi::Persistent(constructorFunction);
  constructor.SuppressDestruct();

  return exports;
}


ODBCStatement::ODBCStatement(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ODBCStatement>(info) {

  this->data = new QueryData();

  this->hENV = *(info[0].As<Napi::External<SQLHENV>>().Data());
  this->hDBC = *(info[1].As<Napi::External<SQLHDBC>>().Data());
  this->data->hSTMT = *(info[2].As<Napi::External<SQLHSTMT>>().Data());
}

ODBCStatement::~ODBCStatement() {
  this->Free();
}

void ODBCStatement::Free() {

  DEBUG_PRINTF("ODBCStatement::Free\n");

  if (this->data->hSTMT) {
    uv_mutex_lock(&ODBC::g_odbcMutex);
    this->data->sqlReturnCode = SQLFreeStmt(this->data->hSTMT, SQL_CLOSE);
    this->data->sqlReturnCode = SQLFreeHandle(SQL_HANDLE_STMT, this->data->hSTMT);
    this->data->hSTMT = NULL;
    delete data;
    uv_mutex_unlock(&ODBC::g_odbcMutex);
  }
}

/******************************************************************************
 ********************************* PREPARE ************************************
 *****************************************************************************/

// PrepareAsyncWorker, used by Prepare function (see below)
class PrepareAsyncWorker : public Napi::AsyncWorker {

  private:
    ODBCStatement *odbcStatementObject;
    QueryData *data;

  public:
    PrepareAsyncWorker(ODBCStatement *odbcStatementObject, Napi::Function& callback) : Napi::AsyncWorker(callback),
    odbcStatementObject(odbcStatementObject),
    data(odbcStatementObject->data) {}

    ~PrepareAsyncWorker() {}

    void Execute() {

      DEBUG_PRINTF("ODBCStatement::PrepareAsyncWorker in Execute()\n");
      
      DEBUG_PRINTF("ODBCStatement::PrepareAsyncWorker hDBC=%X hDBC=%X hSTMT=%X\n",
       odbcStatementObject->hENV,
       odbcStatementObject->hDBC,
       data->hSTMT
      );

      data->sqlReturnCode = SQLPrepare(
        data->hSTMT,
        data->sql, 
        SQL_NTS
      );

      if (!SQL_SUCCEEDED(data->sqlReturnCode)) {
        SetError(ODBC::GetSQLError(SQL_HANDLE_STMT, data->hSTMT, (char *) "[node-odbc] Error in Statement::PrepareAsyncWorker::Execute"));
        return;
      }

      // front-load the work of SQLNumParams and SQLDescribeParam here, so we
      // can convert NAPI/JavaScript values to C values immediately in Bind

      data->sqlReturnCode = SQLNumParams(
        data->hSTMT,
        &data->parameterCount
      );

      if (!SQL_SUCCEEDED(data->sqlReturnCode)) {
        SetError(ODBC::GetSQLError(SQL_HANDLE_STMT, data->hSTMT, (char *) "[node-odbc] Error in Statement::PrepareAsyncWorker::Execute"));
        return;
      }

      data->parameters = new Parameter*[data->parameterCount];
      for (SQLSMALLINT i = 0; i < data->parameterCount; i++) {
        data->parameters[i] = new Parameter();
      }

      data->sqlReturnCode = ODBC::DescribeParameters(data->hSTMT, data->parameters, data->parameterCount);

      if (!SQL_SUCCEEDED(data->sqlReturnCode)) {
        SetError(ODBC::GetSQLError(SQL_HANDLE_STMT, data->hSTMT, (char *) "[node-odbc] Error in Statement::PrepareAsyncWorker::Execute"));
        return;
      }
    }

    void OnOK() {

      DEBUG_PRINTF("ODBCStatement::PrepareAsyncWorker in OnOk()\n");
      DEBUG_PRINTF("ODBCStatement::PrepareAsyncWorker hDBC=%X hDBC=%X hSTMT=%X\n",
       odbcStatementObject->hENV,
       odbcStatementObject->hDBC,
       data->hSTMT
      );

      Napi::Env env = Env();  
      Napi::HandleScope scope(env);

      std::vector<napi_value> callbackArguments;
      callbackArguments.push_back(env.Null());
      callbackArguments.push_back(Napi::Boolean::New(env, true));
      Callback().Call(callbackArguments);
    }
};

/*
 *  ODBCStatement:Prepare (Async)
 *    Description: Prepares an SQL string so that it can be bound with
 *                 parameters and then executed.
 * 
 *    Parameters:
 *      const Napi::CallbackInfo& info:
 *        The information passed by Napi from the JavaScript call, including
 *        arguments from the JavaScript function. In JavaScript, the
 *        prepare() function takes two arguments.
 * 
 *        info[0]: String: the SQL string to prepare.
 *        info[1]: Function: callback function:
 *            function(error, result)
 *              error: An error object if there was a problem getting results,
 *                     or null if operation was successful.
 *              result: The number of rows affected by the executed query.
 * 
 *    Return:
 *      Napi::Value:
 *        Undefined (results returned in callback).
 */
Napi::Value ODBCStatement::Prepare(const Napi::CallbackInfo& info) {

  DEBUG_PRINTF("ODBCStatement::Prepare\n");

  Napi::Env env = info.Env();  
  Napi::HandleScope scope(env);

  if(!info[0].IsString() || !info[1].IsFunction()){
    Napi::TypeError::New(env, "Argument 0 must be a string , Argument 1 must be a function.").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::String sql = info[0].ToString();
  Napi::Function callback = info[1].As<Napi::Function>();

  data->sql = ODBC::NapiStringToSQLTCHAR(sql);

  PrepareAsyncWorker *worker = new PrepareAsyncWorker(this, callback);
  worker->Queue();

  return env.Undefined();
}

/******************************************************************************
 *********************************** BIND *************************************
 *****************************************************************************/

// BindAsyncWorker, used by Bind function (see below)
class BindAsyncWorker : public Napi::AsyncWorker {

  private:

    ODBCStatement *statementObject;
    QueryData *data;

    ~BindAsyncWorker() { }

    void Execute() {
      SQLRETURN sqlReturnCode = ODBC::BindParameters(data->hSTMT, data->parameters, data->parameterCount);

      if (!SQL_SUCCEEDED(sqlReturnCode)) {
        SetError(ODBC::GetSQLError(SQL_HANDLE_STMT, data->hSTMT, (char *) "[node-odbc] Error in Statement::BindAsyncWorker::Bind"));
      }
    }

    void OnOK() {

      DEBUG_PRINTF("\nStatement::BindAsyncWorker::OnOk");

      Napi::Env env = Env();      
      Napi::HandleScope scope(env);

      std::vector<napi_value> callbackArguments;

      callbackArguments.push_back(env.Null());

      Callback().Call(callbackArguments);
    }

  public:

    BindAsyncWorker(ODBCStatement *statementObject, Napi::Function& callback) : Napi::AsyncWorker(callback),
    statementObject(statementObject),
    data(statementObject->data) {}
};

Napi::Value ODBCStatement::Bind(const Napi::CallbackInfo& info) {

  DEBUG_PRINTF("ODBCStatement::Bind\n");
  
  Napi::Env env = info.Env();  
  Napi::HandleScope scope(env);

  if ( !info[0].IsArray() || !info[1].IsFunction() ) {
    Napi::TypeError::New(env, "Function signature is: bind(array, function)").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Array bindArray = info[0].As<Napi::Array>();
  Napi::Function callback = info[1].As<Napi::Function>();

  // if the parameter count isnt right, end right away
  if (data->parameterCount != (SQLSMALLINT)bindArray.Length() || data->parameters == NULL) {
    std::vector<napi_value> callbackArguments;

    Napi::Error error = Napi::Error::New(env, Napi::String::New(env, "[node-odbc] Error in Statement::BindAsyncWorker::Bind: The number of parameters in the prepared statement doesn't match the number of parameters passed to bind."));
    callbackArguments.push_back(error.Value());

    callback.Call(callbackArguments);
    return env.Undefined();
  }

  // converts NAPI/JavaScript values to values used by SQLBindParameter
  ODBC::StoreBindValues(&bindArray, this->data->parameters);

  BindAsyncWorker *worker = new BindAsyncWorker(this, callback);
  worker->Queue();

  return env.Undefined();
}

/******************************************************************************
 ********************************* EXECUTE ************************************
 *****************************************************************************/

// ExecuteAsyncWorker, used by Execute function (see below)
class ExecuteAsyncWorker : public Napi::AsyncWorker {

  private:
    ODBCStatement *odbcStatementObject;
    QueryData *data;

    void Execute() {

      DEBUG_PRINTF("ODBCStatement::ExecuteAsyncWorker::Execute\n");

      data->sqlReturnCode = SQLExecute(data->hSTMT);

      if (SQL_SUCCEEDED(data->sqlReturnCode)) {      
        ODBC::RetrieveData(data);
      } else {
        SetError(ODBC::GetSQLError(SQL_HANDLE_STMT, data->hSTMT, (char *) "[node-odbc] Error in ODBCStatement::ExecuteAsyncWorker::Execute"));
      }
    }

    void OnOK() {

      DEBUG_PRINTF("ODBCStatement::ExecuteAsyncWorker::OnOk()\n");  

      Napi::Env env = Env();
      Napi::HandleScope scope(env);

      Napi::Array rows = ODBC::ProcessDataForNapi(env, data);

      std::vector<napi_value> callbackArguments;
      callbackArguments.push_back(env.Null()); // error is null
      callbackArguments.push_back(rows);

      Callback().Call(callbackArguments);
    }

  public:
    ExecuteAsyncWorker(ODBCStatement *odbcStatementObject, Napi::Function& callback) : Napi::AsyncWorker(callback),
      odbcStatementObject(odbcStatementObject),
      data(odbcStatementObject->data) {}

    ~ExecuteAsyncWorker() {}
};

Napi::Value ODBCStatement::Execute(const Napi::CallbackInfo& info) {
  
  DEBUG_PRINTF("ODBCStatement::Execute\n");

  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Napi::Function callback;

  if (info[0].IsFunction()) { callback = info[0].As<Napi::Function>(); }
  else { Napi::TypeError::New(env, "execute: first argument must be a function").ThrowAsJavaScriptException(); }

  ExecuteAsyncWorker *worker = new ExecuteAsyncWorker(this, callback);
  worker->Queue();

  return env.Undefined();
}

/******************************************************************************
 ********************************** CLOSE *************************************
 *****************************************************************************/

// CloseAsyncWorker, used by Close function (see below)
class CloseAsyncWorker : public Napi::AsyncWorker {

  private:
    ODBCStatement *odbcStatementObject;
    int closeOption;
    QueryData *data;

  public:
    CloseAsyncWorker(ODBCStatement *odbcStatementObject, int closeOption, Napi::Function& callback) : Napi::AsyncWorker(callback),
      odbcStatementObject(odbcStatementObject),
      closeOption(closeOption),
      data(odbcStatementObject->data) {}

    ~CloseAsyncWorker() {}

    void Execute() {

      if (closeOption == SQL_DESTROY) {
        odbcStatementObject->Free();
      } else {
        uv_mutex_lock(&ODBC::g_odbcMutex);
        data->sqlReturnCode = SQLFreeStmt(this->data->hSTMT, closeOption);
        uv_mutex_unlock(&ODBC::g_odbcMutex);
      }

      if (SQL_SUCCEEDED(data->sqlReturnCode)) {
        return;
      } else {
        SetError(ODBC::GetSQLError(SQL_HANDLE_STMT, data->hSTMT, (char *) "[node-odbc] Error in Statement::CloseAsyncWorker::Execute"));
      }

      DEBUG_PRINTF("ODBCStatement::CloseAsyncWorker::Execute()\n");
    }

    void OnOK() {

      DEBUG_PRINTF("ODBCStatement::CloseAsyncWorker::OnOk()\n");  

      Napi::Env env = Env();
      Napi::HandleScope scope(env);

      std::vector<napi_value> callbackArguments;
      callbackArguments.push_back(env.Null());
      Callback().Call(callbackArguments);
    }
};

Napi::Value ODBCStatement::Close(const Napi::CallbackInfo& info) {

  DEBUG_PRINTF("ODBCStatement::Close\n");

  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() != 2 || !info[0].IsNumber() || !info[1].IsFunction()) {
    Napi::TypeError::New(env, "close takes two arguments (closeOption [int], callback [function])").ThrowAsJavaScriptException();
  }

  int closeOption = info[0].ToNumber().Int32Value();
  Napi::Function callback = info[1].As<Napi::Function>(); 

  CloseAsyncWorker *worker = new CloseAsyncWorker(this, closeOption, callback);
  worker->Queue();

  return env.Undefined();
}
