#include "mount/write_executor.h"

#include <cstring>
#include <iostream>

#include "common/cltocs_communication.h"
#include "common/crc.h"
#include "common/cstocl_communication.h"
#include "common/exceptions.h"

const uint32_t kReceiveBufferSize = 1024;

WriteExecutor::WriteExecutor(ChunkserverStats& chunkserverStats,
		const NetworkAddress& headAddress, int headFd,
		uint64_t chunkId, uint32_t chunkVersion, ChunkType chunkType)
		: chunkserverStats_(chunkserverStats),
		  isRunning_(false),
		  chunkId_(chunkId),
		  chunkVersion_(chunkVersion),
		  chunkType_(chunkType),
		  chainHead_(headAddress),
		  chainHeadFd_(headFd),
		  receiveBuffer_(kReceiveBufferSize) {
	chunkserverStats_.registerWriteOperation(chainHead_);
}

WriteExecutor::~WriteExecutor() {
	chunkserverStats_.unregisterWriteOperation(chainHead_);
	for (const auto& server : chain_) {
		chunkserverStats_.unregisterWriteOperation(server);
	}
}

void WriteExecutor::addChunkserverToChain(const NetworkAddress& address) {
	sassert(!isRunning_);
	chain_.push_back(address);
	chunkserverStats_.registerWriteOperation(address);
}

void WriteExecutor::addInitPacket() {
	sassert(!isRunning_);
	pendingPackets_.push_back(Packet());
	std::vector<uint8_t>& buffer = pendingPackets_.back().buffer;
	cltocs::writeInit::serialize(buffer, chunkId_, chunkVersion_, chunkType_, chain_);
	isRunning_ = true;
}

void WriteExecutor::addDataPacket(uint32_t writeId,
		uint16_t block, uint32_t offset, uint32_t size, const uint8_t* data) {
	sassert(isRunning_);
	uint32_t crc = mycrc32(0, data, size);
	pendingPackets_.push_back(Packet());
	Packet& packet = pendingPackets_.back();
	cltocs::writeData::serializePrefix(packet.buffer,
			chunkId_, writeId, block, offset, size, crc);
	packet.data = data;
	packet.dataSize = size;
}

void WriteExecutor::addEndPacket() {
	sassert(isRunning_);
	pendingPackets_.push_back(Packet());
	std::vector<uint8_t>& buffer = pendingPackets_.back().buffer;
	cltocs::writeEnd::serialize(buffer, chunkId_);
}

void WriteExecutor::sendData() {
	if (!bufferWriter_.hasDataToSend()) {
		if (pendingPackets_.empty()) {
			return;
		}
		const Packet& packet = pendingPackets_.front();
		bufferWriter_.addBufferToSend(packet.buffer.data(), packet.buffer.size());
		if (packet.data != nullptr) {
			bufferWriter_.addBufferToSend(packet.data, packet.dataSize);
		}
	}

	ssize_t bytesSent = bufferWriter_.writeTo(chainHeadFd_);
	if (bytesSent == 0) {
		throw ChunkserverConnectionException("Write error: connection closed by peer", server());
	} else if (bytesSent < 0) {
		throw ChunkserverConnectionException("Write error: " + std::string(strerr(errno)), server());
	}
	if (!bufferWriter_.hasDataToSend()) {
		bufferWriter_.reset();
		pendingPackets_.pop_front();
	}
}

std::vector<WriteExecutor::Status> WriteExecutor::receiveData() {
	ssize_t bytesRecv = receiveBuffer_.readFrom(chainHeadFd_);
	if (bytesRecv == 0) {
		throw ChunkserverConnectionException(
				"Read from chunkserver: connection closed by peer", server());
	} else if (bytesRecv < 0 && errno != EINTR) {
		throw ChunkserverConnectionException(
				"Read from chunkserver: " + std::string(strerr(errno)), server());
	}

	std::vector<WriteExecutor::Status> statuses;
	while(receiveBuffer_.hasMessageData()) {
		PacketHeader header = receiveBuffer_.getMessageHeader();
		// TODO deserialize without copying into vector
		std::vector<uint8_t> messageData(receiveBuffer_.getMessageData(),
				receiveBuffer_.getMessageData() + header.length);
		switch (header.type) {
			case LIZ_CSTOCL_WRITE_STATUS:
				statuses.push_back(processStatusMessage(messageData));
				break;
			default:
				throw RecoverableWriteException("Received unknown message from chunkserver ("
						+ std::to_string(header.type) + ")");
		}
		receiveBuffer_.removeMessage();
	}

	return std::move(statuses);
}

WriteExecutor::Status WriteExecutor::processStatusMessage(const std::vector<uint8_t>& message) {
	Status status;
	cstocl::writeStatus::deserialize(message, status.chunkId, status.writeId, status.status);
	return status;
}