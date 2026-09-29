-- CreateSchema
CREATE SCHEMA IF NOT EXISTS "public";

-- CreateTable
CREATE TABLE "Pairing" (
    "mac" TEXT NOT NULL,
    "secret" TEXT NOT NULL,
    "code" TEXT NOT NULL,
    "board" TEXT NOT NULL DEFAULT '',
    "firmware" TEXT NOT NULL DEFAULT '',
    "expiresAt" TIMESTAMP(3) NOT NULL,
    "deviceId" TEXT,

    CONSTRAINT "Pairing_pkey" PRIMARY KEY ("mac")
);

-- CreateTable
CREATE TABLE "Device" (
    "id" TEXT NOT NULL,
    "owner" TEXT NOT NULL,
    "token" TEXT NOT NULL,
    "name" TEXT NOT NULL DEFAULT '',
    "mac" TEXT NOT NULL,
    "board" TEXT NOT NULL DEFAULT '',
    "firmware" TEXT NOT NULL DEFAULT '',
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "lastSeen" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "ip" TEXT NOT NULL DEFAULT '',
    "rssi" INTEGER NOT NULL DEFAULT 0,
    "battery" INTEGER NOT NULL DEFAULT -1,
    "batteryMv" INTEGER NOT NULL DEFAULT 0,
    "charging" BOOLEAN NOT NULL DEFAULT false,
    "heap" INTEGER NOT NULL DEFAULT 0,
    "uptime" INTEGER NOT NULL DEFAULT 0,
    "rev" INTEGER NOT NULL DEFAULT 0,
    "version" INTEGER NOT NULL DEFAULT 0,
    "reported" JSONB NOT NULL,
    "projectSettings" JSONB NOT NULL DEFAULT '{}',
    "pending" JSONB NOT NULL DEFAULT '{}',
    "settingsReported" BOOLEAN NOT NULL DEFAULT false,
    "commands" JSONB NOT NULL DEFAULT '[]',
    "tests" JSONB NOT NULL DEFAULT '{}',
    "testsUpdatedAt" TIMESTAMP(3),
    "samples" JSONB NOT NULL DEFAULT '[]',
    "ota" JSONB,
    "networks" JSONB NOT NULL DEFAULT '[]',

    CONSTRAINT "Device_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "DeviceFile" (
    "id" TEXT NOT NULL,
    "deviceId" TEXT NOT NULL,
    "bytes" BYTEA NOT NULL,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "DeviceFile_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "ProjectFile" (
    "id" TEXT NOT NULL,
    "deviceId" TEXT NOT NULL,
    "name" TEXT NOT NULL,
    "version" TEXT NOT NULL,
    "bytes" BYTEA NOT NULL,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "ProjectFile_pkey" PRIMARY KEY ("id")
);

-- CreateIndex
CREATE INDEX "Pairing_code_idx" ON "Pairing"("code");

-- CreateIndex
CREATE UNIQUE INDEX "Device_token_key" ON "Device"("token");

-- CreateIndex
CREATE INDEX "Device_owner_idx" ON "Device"("owner");

-- CreateIndex
CREATE INDEX "Device_mac_idx" ON "Device"("mac");

-- CreateIndex
CREATE INDEX "DeviceFile_deviceId_createdAt_idx" ON "DeviceFile"("deviceId", "createdAt");

-- CreateIndex
CREATE INDEX "ProjectFile_deviceId_createdAt_idx" ON "ProjectFile"("deviceId", "createdAt");

-- AddForeignKey
ALTER TABLE "DeviceFile" ADD CONSTRAINT "DeviceFile_deviceId_fkey" FOREIGN KEY ("deviceId") REFERENCES "Device"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "ProjectFile" ADD CONSTRAINT "ProjectFile_deviceId_fkey" FOREIGN KEY ("deviceId") REFERENCES "Device"("id") ON DELETE CASCADE ON UPDATE CASCADE;

