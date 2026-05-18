import type { Plugin } from "@opencode-ai/plugin"
import type { Event, Permission } from "@opencode-ai/sdk"
import { BuddyClient } from "./client.js"

let buddyClient: BuddyClient | null = null

function getBuddyClient(): BuddyClient {
    if (!buddyClient) {
        buddyClient = new BuddyClient()
    }
    return buddyClient
}

export const OpenBuddyPlugin: Plugin = async (ctx) => {
    const client = getBuddyClient()
    await client.connect(ctx)

    return {
        event: async ({ event }: { event: Event }) => {
            client.onEvent(event)
        },

        "permission.ask": async (input: Permission, output: { status: "ask" | "deny" | "allow" }) => {
            client.onPermissionAsk(input)
        },

        "tool.execute.before": async (input: { tool: string; sessionID: string; callID: string }, output: { args: unknown }) => {
            client.onToolExecuteBefore(input)
        },

        "tool.execute.after": async (input: { tool: string; sessionID: string; callID: string; args: unknown }, output: { title: string; output: string; metadata: unknown }) => {
            client.onToolExecuteAfter(input)
        },
    }
}
