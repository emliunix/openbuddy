export interface Heartbeat {
    completed?: boolean
    entries?: string[]
    msg?: string
    prompt?: {
        hint: string
        id: string
        tool: string
    }
    running?: number
    tokens?: number
    tokens_today?: number
    total?: number
    waiting?: number
}

export interface TurnEvent {
    content: Array<{ text: string; type: string }>
    evt: "turn"
    role: string
}

export interface BuddyCommand {
    cmd: string
    [key: string]: unknown
}

export interface BuddyPing {
    cmd: "ping"
}

export interface BuddyPermission {
    cmd: "permission"
    decision: "once" | "deny" | "allow"
    id: string
}

export type BuddyMessage = BuddyCommand | BuddyPing | BuddyPermission

export function encodeNdjson(obj: unknown): string {
    return JSON.stringify(obj) + "\n"
}
