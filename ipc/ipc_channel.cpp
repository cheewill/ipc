// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipc/ipc_channel.h"

#include <windows.h>

#include "ipc/ipc_listener.h"
#include "ipc/ipc_utils.h"
#include "ipc/ipc_message.h"
#include <assert.h>
//#include "ipc/ipc_logging.h"
//#include "ipc/ipc_message_utils.h"


namespace
{

// Global atomic used to guarantee channel IDs are unique.
StaticAtomicSequenceNumber g_last_id;

}  // namespace

namespace IPC
{

std::string Channel::GenerateUniqueRandomChannelID()
{
    // Note: the string must start with the current process id, this is how
    // some child processes determine the pid of the parent.
    //
    // This is composed of a unique incremental identifier, the process ID of
    // the creator, an identifier for the child instance, and a strong random
    // component. The strong random component prevents other processes from
    // hijacking or squatting on predictable channel names.
    char buffer[ 64 ] = { 0 }; //10*3 + 2 + 1
    int process_id = ::GetCurrentProcessId();
    sprintf(buffer, "%d.%u.%d", process_id, g_last_id.GetNext(), RandInt(0, (std::numeric_limits<int32>::max)()));
    return std::string(buffer);
}

Channel::State::State(Channel* channel) : is_pending(false)
{
    memset(&context.overlapped, 0, sizeof(context.overlapped));
    context.handler = channel;
}

Channel::State::~State()
{
    //COMPILE_ASSERT(!offsetof(Channel::State, context),
    //               starts_with_io_context);
}

Channel::Channel(const IPC::ChannelHandle &channel_handle,
                 Listener* listener, Thread* thread)
    : ChannelReader(listener),
    input_state_(this),
    output_state_(this),
    pipe_(INVALID_HANDLE_VALUE),
    peer_pid_(0),
    waiting_connect_(true),
    processing_incoming_(false),
    client_secret_(0),
    thread_(thread),
    validate_client_(false)
{
    CreatePipe(channel_handle);
}

Channel::~Channel()
{
    Close();
}

void Channel::Close()
{
    //   if (thread_check_.get()) {
    //     assert(thread_check_->CalledOnValidThread());
    //   }

    if (input_state_.is_pending || output_state_.is_pending)
        CancelIo(pipe_);

    // Closing the handle at this point prevents us from issuing more requests
    // form OnIOCompleted().
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }

    // Make sure all IO has completed.
    //base::Time start = base::Time::Now();
    while (input_state_.is_pending || output_state_.is_pending) {
        thread_->WaitForIOCompletion(INFINITE, this);
    }

    while (!output_queue_.empty()) {
        Message* m = output_queue_.front();
        output_queue_.pop();
        m->Release();
    }
}

bool Channel::Send(Message* message)
{
    //   DCHECK(thread_check_->CalledOnValidThread());
    //   DVLOG(2) << "sending message @" << message << " on channel @" << this
    //            << " with type " << message->type()
    //            << " (" << output_queue_.size() << " in queue)";

#ifdef IPC_MESSAGE_LOG_ENABLED
    Logging::GetInstance()->OnSendMessage(message, "");
#endif
    message->AddRef();
    //message->TraceMessageBegin();
    output_queue_.push(message);
    // ensure waiting to write
    if (!waiting_connect_) {
        if (!output_state_.is_pending) {
            if (!ProcessOutgoingMessages(NULL, 0))
                return false;
        }
    }

    return true;
}



Channel::ReadState Channel::ReadData(
    char* buffer,
    int buffer_len,
    int* /* bytes_read */)
{
    if (INVALID_HANDLE_VALUE == pipe_)
        return READ_FAILED;

    DWORD bytes_read = 0;
    BOOL ok = ReadFile(pipe_, buffer, buffer_len,
                       &bytes_read, &input_state_.context.overlapped);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            input_state_.is_pending = true;
            return READ_PENDING;
        }
        //LOG(ERROR) << "pipe error: " << err;
        return READ_FAILED;
    }

    // We could return READ_SUCCEEDED here. But the way that this code is
    // structured we instead go back to the message loop. Our completion port
    // will be signalled even in the "synchronously completed" state.
    //
    // This allows us to potentially process some outgoing messages and
    // interleave other work on this thread when we're getting hammered with
    // input messages. Potentially, this could be tuned to be more efficient
    // with some testing.
    input_state_.is_pending = true;
    return READ_PENDING;
}

bool Channel::WillDispatchInputMessage(Message* msg)
{
    // Make sure we get a hello when client validation is required.
    if (validate_client_)
        return IsHelloMessage(msg);
    return true;
}

void Channel::HandleHelloMessage(Message* msg)
{
    // The hello message contains one parameter containing the PID.
    MessageReader it(msg);
    int32 claimed_pid;
    bool failed = !it.ReadInt(&claimed_pid);

    if (!failed && validate_client_) {
        int32 secret;
        failed = it.ReadInt(&secret) ? (secret != client_secret_) : true;
    }

    if (failed) {
        assert(0);
        Close();
        listener()->OnChannelError();
        return;
    }

    peer_pid_ = claimed_pid;
    // Validation completed.
    validate_client_ = false;
    listener()->OnChannelConnected(claimed_pid);
}

bool Channel::DidEmptyInputBuffers()
{
    // We don't need to do anything here.
    return true;
}

// static
const std::wstring Channel::PipeName(
    const std::string& channel_id, int32* secret)
{
    std::string name("\\\\.\\pipe\\ipc.");

    // Prevent the shared secret from ending up in the pipe name.
    size_t index = channel_id.find_first_of('\\');
    if (index != std::string::npos) {
        if (secret) {  // Retrieve the secret if asked for.
            *secret = atoi(channel_id.substr(index + 1).c_str());
        }
        return ASCIIToWide(name.append(channel_id.substr(0, index - 1)));
    }

    // This case is here to support predictable named pipes in tests.
    if (secret)
        *secret = 0;
    return ASCIIToWide(name.append(channel_id));
}

bool Channel::CreatePipe(const IPC::ChannelHandle &channel_handle)
{
    assert(INVALID_HANDLE_VALUE == pipe_);
    std::wstring pipe_name;
    // If we already have a valid pipe for channel just copy it.
    if (channel_handle.pipe.handle) {
        assert(channel_handle.name.empty());
        pipe_name = L"Not Available";  // Just used for LOG
        // Check that the given pipe confirms to the specified mode.  We can
        // only check for PIPE_TYPE_MESSAGE & PIPE_SERVER_END flags since the
        // other flags (PIPE_TYPE_BYTE, and PIPE_CLIENT_END) are defined as 0.
        DWORD flags = 0;
        GetNamedPipeInfo(channel_handle.pipe.handle, &flags, NULL, NULL, NULL);
        assert(!(flags & PIPE_TYPE_MESSAGE));
        if (!DuplicateHandle(GetCurrentProcess(),
                             channel_handle.pipe.handle,
                             GetCurrentProcess(),
                             &pipe_,
                             0,
                             FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            //LOG(WARNING) << "DuplicateHandle failed. Error :" << GetLastError();
            return false;
        }
    } else {
        assert(!channel_handle.pipe.handle);
        pipe_name = PipeName(channel_handle.name, &client_secret_);

        //�ȳ��Դ���
        const DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE;
        validate_client_ = !!client_secret_;
        pipe_ = CreateNamedPipeW(pipe_name.c_str(),
                                 open_mode,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
                                 1,
                                 kReadBufferSize,
                                 kReadBufferSize,
                                 5000,
                                 NULL);
        if (pipe_ == INVALID_HANDLE_VALUE) {
            pipe_ = CreateFileW(pipe_name.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION |
                                FILE_FLAG_OVERLAPPED,
                                NULL);

            waiting_connect_ = false;
        }
    }

    if (pipe_ == INVALID_HANDLE_VALUE) {
        // If this process is being closed, the pipe may be gone already.
        //LOG(WARNING) << "Unable to create pipe \"" << pipe_name <<
        //                "\" in " << (mode & MODE_SERVER_FLAG ? "server" : "client")
        //                << " mode. Error :" << GetLastError();
        return false;
    }

    // Create the Hello message to be sent when Connect is called
    Message* m = new Message(MSG_ROUTING_NONE,
                             HELLO_MESSAGE_TYPE,
                             IPC::Message::PRIORITY_NORMAL);
    m->AddRef();
    // Don't send the secret to the untrusted process, and don't send a secret
    // if the value is zero (for IPC backwards compatability).
    int32 secret = validate_client_ ? 0 : client_secret_;
    if (!m->WriteInt(GetCurrentProcessId()) ||
        (secret && !m->WriteUInt32(secret))) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        m->Release();
        return false;
    }

    output_queue_.push(m);
    return true;
}

bool Channel::Connect()
{
    //DLOG_IF(WARNING, thread_check_.get()) << "Connect called more than once";

    //if (!thread_check_.get())
    //  thread_check_.reset(new base::ThreadChecker());

    if (pipe_ == INVALID_HANDLE_VALUE)
        return false;

    thread_->RegisterIOHandler(pipe_, this);

    // Check to see if there is a client connected to our pipe...
    if (waiting_connect_)
        ProcessConnection();

    if (!input_state_.is_pending) {
        // Complete setup asynchronously. By not setting input_state_.is_pending
        // to true, we indicate to OnIOCompleted that this is the special
        // initialization signal.
        thread_->PostTask(
            std::bind(&Channel::OnIOCompleted,
                      this,
                      &input_state_.context,
                      0,
                      0));
    }

    if (!waiting_connect_)
        ProcessOutgoingMessages(NULL, 0);
    return true;
}

bool Channel::ProcessConnection()
{
    //DCHECK(thread_check_->CalledOnValidThread());
    if (input_state_.is_pending)
        input_state_.is_pending = false;

    // Do we have a client connected to our pipe?
    if (INVALID_HANDLE_VALUE == pipe_)
        return false;

    BOOL ok = ConnectNamedPipe(pipe_, &input_state_.context.overlapped);

    DWORD err = GetLastError();
    if (ok) {
        // Uhm, the API documentation says that this function should never
        // return success when used in overlapped mode.
        assert(0);
        return false;
    }

    switch (err) {
    case ERROR_IO_PENDING:
        input_state_.is_pending = true;
        break;
    case ERROR_PIPE_CONNECTED:
        waiting_connect_ = false;
        break;
    case ERROR_NO_DATA:
        // The pipe is being closed.
        return false;
    default:
        assert(0);
        return false;
    }

    return true;
}

bool Channel::ProcessOutgoingMessages(
    Thread::IOContext* context,
    DWORD bytes_written)
{
    assert(!waiting_connect_);  // Why are we trying to send messages if there's
                                // no connection?
    //DCHECK(thread_check_->CalledOnValidThread());

    if (output_state_.is_pending) {
        assert(context);
        output_state_.is_pending = false;
        if (!context || bytes_written == 0) {
            DWORD err = GetLastError();
            //LOG(ERROR) << "pipe error: " << err;
            return false;
        }
        // Message was sent.
        assert(!output_queue_.empty());
        Message* m = output_queue_.front();
        output_queue_.pop();
        m->Release();
    }

    if (output_queue_.empty())
        return true;

    if (INVALID_HANDLE_VALUE == pipe_)
        return false;

    // Write to pipe...
    Message* m = output_queue_.front();
    assert(m->size() <= INT_MAX);
    BOOL ok = WriteFile(pipe_,
                        m->data(),
                        static_cast<int>(m->size()),
                        &bytes_written,
                        &output_state_.context.overlapped);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            output_state_.is_pending = true;

            //DVLOG(2) << "sent pending message @" << m << " on channel @" << this
            //         << " with type " << m->type();

            return true;
        }
        //LOG(ERROR) << "pipe error: " << err;
        return false;
    }

    //DVLOG(2) << "sent message @" << m << " on channel @" << this
    //         << " with type " << m->type();

    output_state_.is_pending = true;
    return true;
}

void Channel::OnIOCompleted(
    Thread::IOContext* context,
    DWORD bytes_transfered,
    DWORD error)
{
    bool ok = true;
    //assert(thread_check_->CalledOnValidThread());
    if (context == &input_state_.context) {
        if (waiting_connect_) {
            if (!ProcessConnection())
                return;
            // We may have some messages queued up to send...
            if (!output_queue_.empty() && !output_state_.is_pending)
                ProcessOutgoingMessages(NULL, 0);
            if (input_state_.is_pending)
                return;
            // else, fall-through and look for incoming messages...
        }

        // We don't support recursion through OnMessageReceived yet!
        assert(!processing_incoming_);
        processing_incoming_ = true;

        // Process the new data.
        if (input_state_.is_pending) {
            // This is the normal case for everything except the initialization step.
            input_state_.is_pending = false;
            if (!bytes_transfered)
                ok = false;
            else if (pipe_ != INVALID_HANDLE_VALUE)
                ok = AsyncReadComplete(bytes_transfered);
        } else {
            assert(!bytes_transfered);
        }

        // Request more data.
        if (ok)
            ok = ProcessIncomingMessages();

        processing_incoming_ = false;
    } else {
        assert(context == &output_state_.context);
        ok = ProcessOutgoingMessages(context, bytes_transfered);
    }
    if (!ok && INVALID_HANDLE_VALUE != pipe_) {
        // We don't want to re-enter Close().
        Close();
        listener()->OnChannelError();
    }
}

// static
bool Channel::IsNamedServerInitialized(
    const std::string& channel_id)
{
    if (WaitNamedPipe(PipeName(channel_id, NULL).c_str(), 1))
        return true;
    // If ERROR_SEM_TIMEOUT occurred, the pipe exists but is handling another
    // connection.
    return GetLastError() == ERROR_SEM_TIMEOUT;
}

// static
std::string Channel::GenerateVerifiedChannelID(const std::string& prefix)
{
    // Windows pipes can be enumerated by low-privileged processes. So, we
    // append a strong random value after the \ character. This value is not
    // included in the pipe name, but sent as part of the client hello, to
    // hijacking the pipe name to spoof the client.

    std::string id = prefix;
    if (!id.empty())
        id.append(".");

    int secret;
    do {  // Guarantee we get a non-zero value.
        secret = RandInt(0, (std::numeric_limits<int>::max)());
    } while (secret == 0);

    id.append(GenerateUniqueRandomChannelID());

    char buffer[ 16 ] = { 0 };
    sprintf(buffer, "\\%d", secret);
    return id.append(buffer);
}

}  // namespace IPC
