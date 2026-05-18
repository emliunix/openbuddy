export type LogLevel = "debug" | "info" | "warn" | "error"

export interface Logger {
    debug(message: string, extra?: Record<string, unknown>): void
    info(message: string, extra?: Record<string, unknown>): void
    warn(message: string, extra?: Record<string, unknown>): void
    error(message: string, extra?: Record<string, unknown>): void
}

const LOG_LEVEL_PRIORITY: Record<LogLevel, number> = {
    debug: 0,
    info: 1,
    warn: 2,
    error: 3,
}

function getConfiguredLogLevel(): LogLevel {
    const env = process.env.OPENBUDDY_LOG_LEVEL
    if (env && env in LOG_LEVEL_PRIORITY) {
        return env as LogLevel
    }
    return "info"
}

export class AppLogger implements Logger {
    private ctx: {
        client: {
            app: {
                log: (req: {
                    body: {
                        service: string
                        level: LogLevel
                        message: string
                        extra: Record<string, unknown>
                    }
                }) => Promise<unknown>
            }
        }
    } | null = null

    private minLevel: LogLevel = getConfiguredLogLevel()
    private buffer: Array<{
        level: LogLevel
        message: string
        extra: Record<string, unknown>
        timestamp: Date
    }> = []
    private bufferLimit = 100
    private consoleFallback = true

    setContext(ctx: {
        client: {
            app: {
                log: (req: {
                    body: {
                        service: string
                        level: LogLevel
                        message: string
                        extra: Record<string, unknown>
                    }
                }) => Promise<unknown>
            }
        }
    }): void {
        this.ctx = ctx
        this.flushBuffer()
    }

    setMinLevel(level: LogLevel): void {
        this.minLevel = level
    }

    setConsoleFallback(enabled: boolean): void {
        this.consoleFallback = enabled
    }

    private shouldLog(level: LogLevel): boolean {
        return LOG_LEVEL_PRIORITY[level] >= LOG_LEVEL_PRIORITY[this.minLevel]
    }

    private async sendLog(
        level: LogLevel,
        message: string,
        extra: Record<string, unknown>
    ): Promise<void> {
        const entry = {
            service: "openbuddy",
            level,
            message,
            extra: {
                ...extra,
                _timestamp: new Date().toISOString(),
            },
        }

        if (this.ctx) {
            try {
                await this.ctx.client.app.log({ body: entry })
            } catch {
                if (this.consoleFallback) {
                    console.error("[OpenBuddy] Failed to send log, falling back to console")
                    this.consoleLog(level, message, extra)
                }
            }
        } else {
            this.buffer.push({ level, message, extra, timestamp: new Date() })
            if (this.buffer.length > this.bufferLimit) {
                this.buffer.shift()
            }
            if (this.consoleFallback) {
                this.consoleLog(level, message, extra)
            }
        }
    }

    private consoleLog(
        level: LogLevel,
        message: string,
        extra: Record<string, unknown>
    ): void {
        const prefix = `[OpenBuddy:${level.toUpperCase()}]`
        if (Object.keys(extra).length > 0) {
            console.log(prefix, message, extra)
        } else {
            console.log(prefix, message)
        }
    }

    private flushBuffer(): void {
        const buffered = this.buffer
        this.buffer = []
        for (const entry of buffered) {
            this.sendLog(entry.level, entry.message, entry.extra)
        }
    }

    debug(message: string, extra?: Record<string, unknown>): void {
        if (!this.shouldLog("debug")) return
        this.sendLog("debug", message, extra || {})
    }

    info(message: string, extra?: Record<string, unknown>): void {
        if (!this.shouldLog("info")) return
        this.sendLog("info", message, extra || {})
    }

    warn(message: string, extra?: Record<string, unknown>): void {
        if (!this.shouldLog("warn")) return
        this.sendLog("warn", message, extra || {})
    }

    error(message: string, extra?: Record<string, unknown>): void {
        if (!this.shouldLog("error")) return
        this.sendLog("error", message, extra || {})
    }
}
